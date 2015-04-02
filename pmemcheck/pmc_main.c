/*
 * Persistent memory checker.
 * Copyright (c) 2014-2015, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, or (at your option) any later version, as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

/*
 * This program is based on lackey, cachegrind and memcheck.
 */

#include <sys/param.h>
#include <stdio.h>
#include "pub_tool_libcfile.h"
#include <fcntl.h>
#include "libvex_ir.h"
#include "pub_tool_oset.h"
#include "pub_tool_mallocfree.h"
#include "pub_tool_options.h"
#include "pub_tool_libcbase.h"
#include "pub_tool_gdbserver.h"
#include "pub_tool_tooliface.h"
#include "pub_tool_threadstate.h"
#include "pub_tool_libcassert.h"
#include "pub_tool_libcprint.h"
#include "pub_tool_machine.h"

#include "pmemcheck.h"

/* track at max this many multiple overwrites */
#define MAX_MULT_OVERWRITES 10000UL

/* track at max this many flush error events */
#define MAX_FLUSH_ERROR_EVENTS 10000UL

/* build various kinds of expressions */
#define triop(_op, _arg1, _arg2, _arg3) \
                                 IRExpr_Triop((_op),(_arg1),(_arg2),(_arg3))
#define binop(_op, _arg1, _arg2) IRExpr_Binop((_op),(_arg1),(_arg2))
#define unop(_op, _arg)          IRExpr_Unop((_op),(_arg))
#define mkU1(_n)                 IRExpr_Const(IRConst_U1(_n))
#define mkU8(_n)                 IRExpr_Const(IRConst_U8(_n))
#define mkU16(_n)                IRExpr_Const(IRConst_U16(_n))
#define mkU32(_n)                IRExpr_Const(IRConst_U32(_n))
#define mkU64(_n)                IRExpr_Const(IRConst_U64(_n))
#define mkexpr(_tmp)             IRExpr_RdTmp((_tmp))

/** Max store size */
#define MAX_DSIZE    256

/** Max allowable path length */
#define MAX_PATH_SIZE 4096

/** Single store to memory. */
struct pmem_st {
    Addr addr;
    ULong size;
    ULong block_num;
    UWord value;
    ExeContext *context;
    enum store_state {
        STST_CLEAN,
        STST_DIRTY,
        STST_FLUSHED,
        STST_FENCED,
        STST_COMMITED
    } state;
};

/** Holds parameters and runtime data */
static struct pmem_ops {
    /** Set of stores to persistent memory. */
    OSet *pmem_stores;

    /** Set of registered persistent memory regions. */
    OSet *pmem_mappings;

    /** Set of registered loggable persistent memory regions. */
    OSet *loggable_regions;

    /** Holds possible multiple overwrite error events. */
    struct pmem_st **multiple_stores;

    /** Holds the number of registered multiple overwrites. */
    UWord multiple_stores_reg;

    /** Holds possible flush error events. */
    struct pmem_st **flush_errors;

    /** Holds the number of registered erroneous flush events. */
    UWord flush_errors_reg;

    /** Within this many SBlocks a consecutive write is not considered
    * a poss_leak. */
    UWord store_sb_indiff;

    /** Turns on multiple overwrite error tracking. */
    Bool track_multiple_stores;

    /** Turns on logging persistent memory events. */
    Bool log_stores;

    /** Toggles logging on user requests */
    Bool loggin_on;

    /** Toggles summary printing. */
    Bool print_summary;

    /** Toggles checking multiple flushes of stores */
    Bool check_flush;

    /** The size of the cache line */
    Long flush_align;
} pmem;

/*
 * Memory tracing pattern as in cachegrind/lackey - in case of future
 * improvements.
 */

/** A specific kind of expression. */
typedef IRExpr IRAtom;

/** Types of discernable events. */
typedef enum {
    Event_Ir,
    Event_Dr,
    Event_Dw,
    Event_Dm
} EventKind;

/** The event structure. */
typedef struct {
    EventKind ekind;
    IRAtom *addr;
    SizeT size;
    IRAtom *guard; /* :: Ity_I1, or NULL=="always True" */
    IRAtom *value;
} Event;

/** Number of sblock run. */
static ULong sblocks = 0;

/**
* \brief Check if the given region is in the set.
* \param[in] region Region to find.
* \param[in] region_set Region set to check.
* \return 0 if not in set, 1 if fully in the set, 2 if it overlaps over
* the head of an existing mapping, 3 if it overlaps over the tail of an existing
* mapping.
*/
static UWord
is_in_mapping_set(struct pmem_st *region, OSet *region_set)
{
    if (!VG_(OSetGen_Contains)(region_set, region)) {
        /* not in set */
        return 0;
    }

    struct pmem_st *found_region = VG_(OSetGen_Lookup)(region_set, region);

    if (region->addr < found_region->addr)
        /* front overlaps */
        return 2;
    else if ((region->addr + region->size) > (found_region->addr +
            found_region->size))
        /* tail overlaps */
        return 3;

    /* region fully within the mapping */
    return 1;
}

/**
* \brief Adds a region to a set.
*
* Overlapping regions will be merged.
* \param[in] region The region to register.
* \param[in, out] region_set The region set to which region will be registered.
*/
static void
add_region(struct pmem_st *region, OSet *region_set)
{
    struct pmem_st *entry = VG_(OSetGen_AllocNode(region_set,
            (SizeT)sizeof (struct pmem_st)));
    entry->addr = region->addr;
    entry->size = region->size;
    entry->state = STST_CLEAN;

    struct pmem_st *old_entry;
    while((old_entry = VG_(OSetGen_Remove)(region_set, entry)) != NULL) {
        /* registering overlapping memory regions, glue them together */
        ULong max_addr = MAX(entry->addr + entry->size, old_entry->addr +
                old_entry->size);
        entry->addr = MIN(entry->addr, old_entry->addr);
        entry->size = max_addr - entry->addr;
        VG_(OSetGen_FreeNode)(region_set, old_entry);
    }
    VG_(OSetGen_Insert)(region_set, entry);
}

/**
* \brief Removes a region from a set.
*
* Partial overlaps will remove only the overlapping parts. For example if you
* have two regions registered (0x100-0x140) and (0x150-0x200) and you remove
* (0x130-0x160) you will end up with two regions (0x100-0x130)
* and (0x160-0x200).
* \param[in] region The region to remove.
* \param[in, out] region_set The region set to which region will be removed.
*/
static void
remove_region(struct pmem_st *region, OSet *region_set)
{
    struct pmem_st *modified_entry = NULL;
    SizeT region_max_addr = region->addr + region->size;
    while ((modified_entry = VG_(OSetGen_Remove)(region_set, region)) !=
            NULL) {
        SizeT mod_entry_max_addr = modified_entry->addr + modified_entry->size;
        if ((modified_entry->addr > region->addr) && (mod_entry_max_addr <
                region_max_addr)) {
            /* modified entry fully within removed region */
            VG_(OSetGen_FreeNode)(region_set, modified_entry);
        } else if ((modified_entry->addr < region->addr) &&
                (mod_entry_max_addr > region_max_addr)) {
            /* modified entry is larger than the removed region - slice */
            modified_entry->size = region->addr - modified_entry->addr;
            VG_(OSetGen_Insert)(region_set, modified_entry);
            struct pmem_st *new_region = VG_(OSetGen_AllocNode)(region_set,
                    sizeof (struct pmem_st));
            new_region->addr = region_max_addr;
            new_region->size = mod_entry_max_addr - new_region->addr;
            VG_(OSetGen_Insert)(region_set, new_region);
        } else if (modified_entry->addr > region->addr) {
            /* head overlaps */
            modified_entry->addr = region_max_addr;
            VG_(OSetGen_Insert)(region_set, modified_entry);
        } else if (mod_entry_max_addr < region_max_addr) {
            /* tail overlaps */
            modified_entry->size = region->addr - modified_entry->addr;
            VG_(OSetGen_Insert)(region_set, modified_entry);
        } else {
            /* exact match */
            VG_(OSetGen_FreeNode)(region_set, modified_entry);
        }
    }
}

/**
* \brief A compare function for regions stored in the OSetGen.
* \param[in] key The key to compare.
* \param[in] elem The element to compare with.
* \return -1 if key is smaller, 1 if key is greater and 0 if key is equal
* to elem.
*/
static Word cmp_pmem_st(const void *key, const void *elem) {
    struct pmem_st *lhs = (struct pmem_st *) (key);
    struct pmem_st *rhs = (struct pmem_st *) (elem);

    if (lhs->addr + lhs->size <= rhs->addr)
        return -1;
    else if (lhs->addr >= rhs->addr + rhs->size)
        return 1;
    else
        return 0;
}

/**
* \brief Checks if a given store overlaps with registered persistent memory
*        regions.
* \param[in] addr The base address of the store.
* \param[in] size The size of the store.
* \return True if store overlaps with any registered region, false otherwise.
*/
static Bool
is_pmem_access(Addr addr, SizeT size)
{
    struct pmem_st tmp;
    tmp.size = size;
    tmp.addr = addr;
    return VG_(OSetGen_Contains)(pmem.pmem_mappings, &tmp);
}

/**
* \brief Prints the error message for exceeding the maximum allowable
*        overwrites.
*/
static void
print_max_poss_overwrites_error(void)
{
    VG_(umsg)("The number of overwritten stores exceeded %lu\n\n",
            MAX_MULT_OVERWRITES);

    VG_(umsg)("This either means there is something fundamentally wrong with"
            " your program,\nor you are using your persistent memory as "
            "volatile memory.");
}

/**
* \brief Traces the given store if it was to any of the registered persistent
*        memory regions
* \param[in] addr The base address of the store.
* \param[in] size The size of the store.
* \param[in] value The value of the store.
*/
static VG_REGPARM(3) void
trace_pmem_store(Addr addr, SizeT size, UWord value)
{
    if(LIKELY(!is_pmem_access(addr, size)))
        return;

    struct pmem_st *store = VG_(OSetGen_AllocNode)(pmem.pmem_stores,
            (SizeT) sizeof (struct pmem_st));
    store->addr = addr;
    store->size = size;
    store->state = STST_DIRTY;
    store->block_num = sblocks;
    store->value = value;
    store->context = VG_(record_ExeContext)(VG_(get_running_tid)(), 0);

    /* log the store, regardless if it is a double store */
    if (pmem.log_stores && ( pmem.loggin_on || VG_(OSetGen_Contains)
            (pmem.loggable_regions, store)))
        VG_(emit)("|STORE;0x%lx;0x%lx;0x%lx", addr, value, size);

    struct pmem_st *existing;
    while ((existing = VG_(OSetGen_Lookup)(pmem.pmem_stores, store)) !=
            NULL) {
        VG_(OSetGen_Remove)(pmem.pmem_stores, existing);
        /* not tracking multiple stores, remove and move on */
        if (LIKELY(!pmem.track_multiple_stores)) {
            VG_(OSetGen_FreeNode)(pmem.pmem_stores, existing);
            continue;
        }

        /* check store indifference */
        if((store->block_num - existing->block_num) < pmem.store_sb_indiff
                && existing->addr == store->addr
                && existing->size == store->size
                && existing->value == store->value) {
            VG_(OSetGen_FreeNode)(pmem.pmem_stores, existing);
            continue;
        } else {
            if (UNLIKELY(pmem.multiple_stores_reg == MAX_MULT_OVERWRITES)) {
                print_max_poss_overwrites_error();
                VG_(exit)(-1);
            }
            /* register the old store as a possible leak */
            pmem.multiple_stores[pmem.multiple_stores_reg++] = existing;
        }
    }
    /* it is now safe to insert the new store */
    VG_(OSetGen_Insert)(pmem.pmem_stores, store);
}

/**
* \brief Register the entry of a new SB.
*
* Useful when handling implementation independent multiple writes under
* the same address.
*/
static void
add_one_SB_entered(void)
{
    ++sblocks;
}

/**
* \brief Makes a new atomic expression from e.
*
* A very handy function to have for creating binops, triops and widens.
* \param[in,out] sb The IR superblock to which the new expression will be added.
* \param[in] ty The IRType of the expression.
* \param[in] e The new expression to make.
* \return The Rd_tmp of the new expression.
*/
static IRAtom *
make_expr(IRSB *sb, IRType ty, IRExpr *e)
{
    IRTemp t;
    IRType tyE = typeOfIRExpr(sb->tyenv, e);

    tl_assert(tyE == ty); /* so 'ty' is redundant (!) */

    t = newIRTemp(sb->tyenv, tyE);
    addStmtToIRSB(sb, IRStmt_WrTmp(t,e));

    return mkexpr(t);
}

/**
* \brief Checks if the expression needs to be widened.
* \param[in] sb The IR superblock to which the expression belongs.
* \param[in] e The checked expression.
* \return True if needs to be widened, false otherwise.
*/
static Bool
tmp_needs_widen(IRSB *sb, IRAtom *e)
{
    switch (typeOfIRExpr(sb->tyenv, e)) {
        case Ity_I1:
        case Ity_I8:
        case Ity_I16:
        case Ity_I32:
            return True;

        default:
            return False;
    }
}

/**
* \brief Checks if the const expression needs to be widened.
* \param[in] e The checked expression.
* \return True if needs to be widened, false otherwise.
*/
static Bool
const_needs_widen(IRAtom *e)
{
    /* make sure this is a const */
    tl_assert(e->tag == Iex_Const);

    switch (e->Iex.Const.con->tag) {
        case Ico_U1:
        case Ico_U8:
        case Ico_U16:
        case Ico_U32:
        case Ico_U64:
            return True;

        default:
            return False;
    }
}

/**
* \brief Widens a given const expression to a word sized expression.
* \param[in] e The expression being widened.
* \return The widened const expression.
*/
static IRAtom *
widen_const(IRAtom *e)
{
    /* make sure this is a const */
    tl_assert(e->tag == Iex_Const);

    switch (e->Iex.Const.con->tag) {
        case Ico_U1:
            return mkIRExpr_HWord((UInt)e->Iex.Const.con->Ico.U1);

        case Ico_U8:
            return mkIRExpr_HWord((UInt)e->Iex.Const.con->Ico.U8);

        case Ico_U16:
            return mkIRExpr_HWord((UInt)e->Iex.Const.con->Ico.U16);

        case Ico_U32:
            return mkIRExpr_HWord((UInt)e->Iex.Const.con->Ico.U32);

        case Ico_U64:
            return mkIRExpr_HWord((UInt)e->Iex.Const.con->Ico.U64);

        default:
            tl_assert(False); /* cannot happen */
    }
}

/**
* \brief A generic widening function.
* \param[in] sb The IR superblock to which the expression belongs.
* \param[in] e The expression being widened.
* \return The widening operation.
*/
static IROp
widen_operation(IRSB *sb, IRAtom *e)
{
    switch (typeOfIRExpr(sb->tyenv, e)) {
        case Ity_I1:
            return Iop_1Uto64;

        case Ity_I8:
            return Iop_8Uto64;

        case Ity_I16:
            return Iop_16Uto64;

        case Ity_I32:
            return Iop_32Uto64;

        default:
            tl_assert(False); /* cannot happen */
    }
}

/**
* \brief Handles wide sse operations.
* \param[in,out] sb The IR superblock to which add expressions.
* \param[in] end The endianess.
* \param[in] addr The expression with the address of the operation.
* \param[in] data The expression with the value of the operation.
* \param[in] guard The guard expression.
* \param[in] size The size of the operation.
*/
static void
handle_wide_expr(IRSB *sb, IREndness end, IRAtom *addr, IRAtom *data,
        IRAtom *guard, SizeT size)
{
    IROp mkAdd;
    IRType ty, tyAddr;
    void *helper = trace_pmem_store;
    const HChar *hname = "trace_pmem_store";

    ty = typeOfIRExpr(sb->tyenv, data);

    tyAddr = typeOfIRExpr(sb->tyenv, addr);
    mkAdd = tyAddr==Ity_I32 ? Iop_Add32 : Iop_Add64;
    tl_assert( tyAddr == Ity_I32 || tyAddr == Ity_I64 );
    tl_assert( end == Iend_LE || end == Iend_BE );

    Int i;
    Int parts = 0;
    /* These are the offsets of the parts in memory. */
    UInt offs[4];

    /* Various bits for constructing the 4/2 lane helper calls */
    IROp ops[4];
    IRDirty *dis[4];
    IRAtom *addrs[4];
    IRAtom *datas[4];
    IRAtom *eBiass[4];

    if (ty == Ity_V256) {
         /* V256-bit case -- phrased in terms of 64 bit units (Qs), with
           Q3 being the most significant lane. */

        ops[0] =Iop_V256to64_0;
        ops[1] =Iop_V256to64_1;
        ops[2] =Iop_V256to64_2;
        ops[3] = Iop_V256to64_3;

        if (end == Iend_LE) {
            offs[0] = 0; offs[1] = 8; offs[2] = 16; offs[3] = 24;
        } else {
            offs[3] = 0; offs[2] = 8; offs[1] = 16; offs[0] = 24;
        }

        parts = 4;
    } else if (ty == Ity_V128) {

        /* V128-bit case
           See comment in next clause re 64-bit regparms also, need to be
           careful about endianness */
        ops[0] =Iop_V128to64;
        ops[1] =Iop_V128HIto64;

        if (end == Iend_LE) {
            offs[0] = 0; offs[1] = 8;
        } else {
            offs[0] = 8; offs[1] = 0;
        }

        parts = 2;
    }

    for(i = 0; i < parts; ++i) {
        eBiass[i] = tyAddr == Ity_I32 ? mkU32(offs[i]) : mkU64(offs[i]);
        addrs[i] = make_expr(sb, tyAddr, binop(mkAdd, addr, eBiass[i]));
        datas[i] = make_expr(sb, Ity_I64, unop(ops[i], data));
        dis[i] = unsafeIRDirty_0_N(3/*regparms*/, hname,
                VG_(fnptr_to_fnentry)(helper), mkIRExprVec_3(addrs[i],
                mkIRExpr_HWord(size / parts), datas[i]));
        if (guard)
            dis[i]->guard = guard;

        addStmtToIRSB(sb, IRStmt_Dirty(dis[i]));
    }
}

/**
* \brief Add a guarded write event.
* \param[in,out] sb The IR superblock to which the expression belongs.
* \param[in] daddr The expression with the address of the operation.
* \param[in] dsize The size of the operation.
* \param[in] guard The guard expression.
* \param[in] value The expression with the value of the operation.
*/
static void
add_event_dw_guarded(IRSB *sb, IRAtom *daddr, Int dsize, IRAtom *guard,
        IRAtom *value)
{
    tl_assert(isIRAtom(daddr));
    tl_assert(isIRAtom(value));
    tl_assert(dsize >= 1 && dsize <= MAX_DSIZE);

    const HChar *helperName = "trace_pmem_store";
    void *helperAddr = trace_pmem_store;
    IRExpr **argv;
    IRDirty *di;

    if (value->tag == Iex_RdTmp
            && typeOfIRExpr(sb->tyenv, value) == Ity_I64) {
        /* handle the normal case */
        argv = mkIRExprVec_3(daddr, mkIRExpr_HWord(dsize),
                value);
        di = unsafeIRDirty_0_N(/*regparms*/3, helperName,
                VG_(fnptr_to_fnentry)(helperAddr), argv);
        if (guard) {
            di->guard = guard;
        }
        addStmtToIRSB(sb, IRStmt_Dirty(di));
    } else if ( value->tag == Iex_RdTmp && tmp_needs_widen(sb, value)) {
        /* the operation needs to be widened */
        argv = mkIRExprVec_3(daddr, mkIRExpr_HWord(dsize),
                make_expr(sb, Ity_I64, unop(widen_operation(sb, value),
                        value)));
        di = unsafeIRDirty_0_N(/*regparms*/3, helperName,
                VG_(fnptr_to_fnentry)(helperAddr), argv);
        if (guard) {
            di->guard = guard;
        }
        addStmtToIRSB(sb, IRStmt_Dirty(di));
    } else if (value->tag == Iex_Const && const_needs_widen(value)) {
        /* the operation needs to be widened */
        argv = mkIRExprVec_3(daddr, mkIRExpr_HWord(dsize),
                widen_const(value));
        di = unsafeIRDirty_0_N(/*regparms*/3, helperName,
                VG_(fnptr_to_fnentry)(helperAddr), argv);
        if (guard) {
            di->guard = guard;
        }
        addStmtToIRSB(sb, IRStmt_Dirty(di));
    } else if (typeOfIRExpr(sb->tyenv, value) == Ity_V128 ||
            typeOfIRExpr(sb->tyenv, value) == Ity_V256 ) {
        handle_wide_expr(sb, Iend_LE, daddr, value, guard, dsize);
    } else {
        VG_(umsg)("Unable to trace store - unsupported type of store\n");
    }
}

/**
* \brief Add an ordinary write event.
* \param[in,out] sb The IR superblock to which the expression belongs.
* \param[in] daddr The expression with the address of the operation.
* \param[in] dsize The size of the operation.
* \param[in] value The expression with the value of the operation.
*/
static void
add_event_dw(IRSB *sb, IRAtom *daddr, Int dsize, IRAtom *value)
{
    add_event_dw_guarded(sb, daddr, dsize, NULL, value);
}

/**
* \brief Register a fence.
*
* Marks flushed stores as fenced and commited stores as persistent.
* The proper state transitions are DIRTY->FLUSHED->FENCED->COMMITED->CLEAN.
* The CLEAN state is not registered, the store is removed from the set.
*/
static void
do_fence(void)
{
    if (pmem.log_stores && (pmem.loggin_on
            || (VG_(OSetGen_Size)(pmem.loggable_regions) != 0)))
        VG_(emit)("|FENCE");

    /* go through the stores and move them from flushed to fenced */
    VG_(OSetGen_ResetIter)(pmem.pmem_stores);
    struct pmem_st *being_fenced = NULL;
    while ((being_fenced = VG_(OSetGen_Next)(pmem.pmem_stores)) != NULL) {
        if (being_fenced->state == STST_FLUSHED) {
            being_fenced->state = STST_FENCED;
        } else if (being_fenced->state == STST_COMMITED) {
            /* remove it from the oset */
            struct pmem_st temp = *being_fenced;
            VG_(OSetGen_Remove)(pmem.pmem_stores, being_fenced);
            VG_(OSetGen_FreeNode)(pmem.pmem_stores, being_fenced);
            /* reset the iterator (remove invalidated store) */
            VG_(OSetGen_ResetIterAt)(pmem.pmem_stores, &temp);
        }
    }
}

/**
* \brief Register a memory commit.
*
* Marks fenced stores as commited. To make commited stores persistent
* for sure, a fence is needed afterwards. The proper state transitions
* are DIRTY->FLUSHED->FENCED->COMMITED->CLEAN. The CLEAN state is not
* registered, the store is removed from the set.
*/
static void
do_commit(void)
{
    if (pmem.log_stores && (pmem.loggin_on
            || (VG_(OSetGen_Size)(pmem.loggable_regions) != 0)))
        VG_(emit)("|COMMIT");
    /* go through the stores and move them from fenced to clean */
    VG_(OSetGen_ResetIter)(pmem.pmem_stores);
    struct pmem_st *being_fenced = NULL;
    while ((being_fenced = VG_(OSetGen_Next)(pmem.pmem_stores)) != NULL) {
        if (being_fenced->state == STST_FENCED)
            being_fenced->state = STST_COMMITED;
    }
}

/**
* \brief Register a flush.
*
* Marks dirty stores as flushed. The proper state transitions are
* DIRTY->FLUSHED->FENCED->COMMITED->CLEAN. The CLEAN state is not registered,
* the store is removed from the set.
*
* \param[in] base The base address of the flush.
* \param[in] size The size of the flush in bytes.
*/
static void
do_flush(UWord base, UWord size)
{
    struct pmem_st flush_info;
    /* Align flushed memory */
    flush_info.addr = base & ~(pmem.flush_align - 1);
    flush_info.size = roundup(size, pmem.flush_align);

    if (pmem.log_stores && (pmem.loggin_on
            || (VG_(OSetGen_Size)(pmem.loggable_regions) != 0)))
        VG_(emit)("|FLUSH;0x%lx;0x%llx", flush_info.addr, flush_info.size);

    /* unfortunately lookup doesn't work here, the oset is an avl tree */

    /* reset the iterator */
    VG_(OSetGen_ResetIter)(pmem.pmem_stores);
    Addr flush_max = flush_info.addr + flush_info.size;
    struct pmem_st *being_flushed;
    while ((being_flushed = VG_(OSetGen_Next)(pmem.pmem_stores)) != NULL){

       /* not an interesting entry, flush doesn't matter */
       if (cmp_pmem_st(&flush_info, being_flushed) != 0) {
           continue;
       }

       /* check for multiple flushes of stores */
       if (being_flushed->state != STST_DIRTY) {
           if (pmem.check_flush) {
               /* multiple flush of the same store - probably an issue */
               pmem.flush_errors[pmem.flush_errors_reg] =
                       VG_(malloc)("pmc.main.cpci.2", sizeof(struct pmem_st));
               *(pmem.flush_errors[pmem.flush_errors_reg]) = *being_flushed;
               ++pmem.flush_errors_reg;
           }
           continue;
       }

       being_flushed->state = STST_FLUSHED;

       /* store starts before base flush address */
       if (being_flushed->addr < flush_info.addr) {
            /* split and reinsert */
            struct pmem_st *split = VG_(OSetGen_AllocNode)(pmem.pmem_stores,
                    (SizeT)sizeof (struct pmem_st));
            split->addr = being_flushed->addr;
            split->size = flush_info.addr - being_flushed->addr;
            split->state = STST_DIRTY;
            /* adjust original */
            VG_(OSetGen_Remove)(pmem.pmem_stores, being_flushed);
            being_flushed->addr = flush_info.addr;
            being_flushed->size -= split->size;
            VG_(OSetGen_Insert)(pmem.pmem_stores, being_flushed);
            /* reset iter */
            VG_(OSetGen_ResetIterAt)(pmem.pmem_stores, being_flushed);
       }

       /* end of store is behind max flush */
       if (being_flushed->addr + being_flushed->size > flush_max) {
            /* split and reinsert */
            struct pmem_st *split = VG_(OSetGen_AllocNode)(pmem.pmem_stores,
                    (SizeT)sizeof (struct pmem_st));
            split->addr = flush_max;
            split->size = being_flushed->addr + being_flushed->size - flush_max;
            split->state = STST_DIRTY;

            /* adjust original */
            VG_(OSetGen_Remove)(pmem.pmem_stores, being_flushed);
            being_flushed->size -= split->size;
            VG_(OSetGen_Insert)(pmem.pmem_stores, split);
            VG_(OSetGen_Insert)(pmem.pmem_stores, being_flushed);
            /* reset iter */
            VG_(OSetGen_ResetIterAt)(pmem.pmem_stores, split);
       }
    }
}

/**
* \brief State to string change for information purposes.
*/
static const char *
store_state_to_string(enum store_state state)
{
    switch (state) {
        case STST_CLEAN:
            return "CLEAN";
        case STST_DIRTY:
            return "DIRTY";
        case STST_FLUSHED:
            return "FLUSHED";
        case STST_FENCED:
            return "FENCED";
        case STST_COMMITED:
            return "COMMITED";
        default:
            return NULL;
    }
}

/**
* \brief Reads the cache line size - linux specific.
* \return The size of the cache line.
*/
static Int
read_cache_line_size(void)
{
    /* the assumed cache line size */
    Int ret_val = 64;

    int fp;
    if ((fp = VG_(fd_open)("/proc/cpuinfo",O_RDONLY, 0)) < 0) {
        return ret_val;
    }

    int proc_read_size = 2048;
    char read_buffer[proc_read_size];

    while (VG_(read)(fp, read_buffer, proc_read_size - 1) > 0) {
        static const char clflush[] = "clflush size\t: ";
        read_buffer[proc_read_size] = 0;

        char *cache_str = NULL;
        if ((cache_str = VG_(strstr)(read_buffer, clflush)) != NULL) {
            /* move to cache line size */
            cache_str += sizeof (clflush) - 1;
            ret_val = VG_(strtoll10)(cache_str, NULL) ? : 64;
            break;
        }
    }

    VG_(close)(fp);
    return ret_val;
}

/**
* \brief Tries to register a file mapping.
* \param[in] fd The file descriptor to be registered.
* \param[in] addr The address at which this file will be mapped.
* \param[in] size The size of the registered file mapping.
* \return Returns 1 on success, 0 otherwise.
*/
static UInt
register_new_file(Int fd, UWord base, UWord size, UWord offset)
{
    char fd_path[64];
    VG_(sprintf(fd_path, "/proc/self/fd/%d", fd));
    UInt retval = 1;

    char *file_name = VG_(malloc)("pmc.main.nfcc", MAX_PATH_SIZE);
    int read_length = VG_(readlink)(fd_path, file_name, MAX_PATH_SIZE - 1);
    if (read_length <= 0) {
        retval = 0;
        goto out;
    }

    file_name[read_length] = 0;

    /* logging_on shall have no effect on this */
    if (pmem.log_stores)
        VG_(emit)("|REGISTER_FILE;%s;0x%lx;0x%lx;0x%lx", file_name, base,
                size, offset);
out:
    VG_(free)(file_name);
    return retval;
}

/**
* \brief Print tool statistics.
*/
static void
print_pmem_stats(void)
{
    VG_(umsg)("Number of stores not made persistent: %lu\n", VG_(OSetGen_Size)
            (pmem.pmem_stores));

    if (VG_(OSetGen_Size)(pmem.pmem_stores) != 0) {
        VG_(OSetGen_ResetIter)(pmem.pmem_stores);
        struct pmem_st *tmp = NULL;
        UWord total = 0;
        Int i = 0;
        VG_(umsg)("Stores not made persistent properly:\n");
        while ((tmp = VG_(OSetGen_Next)(pmem.pmem_stores)) != NULL) {
            VG_(umsg)("[%d] ", i);
            VG_(pp_ExeContext)(tmp->context);
            VG_(umsg)("\tAddress: 0x%lx\tsize: %llu\tstate: %s\n",
                    tmp->addr, tmp->size, store_state_to_string(tmp->state));
            total += tmp->size;
            ++i;
        }
        VG_(umsg)("Total memory not made persistent: %lu\n", total);
    }

    if(pmem.flush_errors_reg) {
        VG_(umsg)("\nNumber of multiply flushed stores: %lu\n",
                pmem.flush_errors_reg);
        VG_(umsg)("Stores flushed multiple times:\n");
        struct pmem_st *tmp = NULL;
        Int i;
        for (i = 0; i < pmem.flush_errors_reg; ++i) {
            tmp = pmem.flush_errors[i];
            VG_(umsg)("[%d] ", i);
            VG_(pp_ExeContext)(tmp->context);
            VG_(printf)("\tAddress: 0x%lx\tsize: %llu\tstate: %s\n",
                    tmp->addr, tmp->size, store_state_to_string(tmp->state));
        }
    }

    if (pmem.track_multiple_stores && (pmem.multiple_stores_reg > 0)) {
        VG_(umsg)("\nNumber of overwritten stores: %lu\n",
                pmem.multiple_stores_reg);
        VG_(umsg)("Overwritten stores before they were made persistent:\n");
        struct pmem_st *tmp = NULL;
        Int i;
        for (i = 0; i < pmem.multiple_stores_reg; ++i) {
            tmp = pmem.multiple_stores[i];
            VG_(umsg)("[%d] ", i);
            VG_(pp_ExeContext)(tmp->context);
            VG_(printf)("\tAddress: 0x%lx\tsize: %llu\tstate: %s\n",
                    tmp->addr, tmp->size, store_state_to_string(tmp->state));
        }
    }
}

/**
* \brief Prints the registered persistent memory mappings
*/
static void
print_persistent_mappings(void)
{
    VG_(OSetGen_ResetIter)(pmem.pmem_mappings);
    struct pmem_st *mapping;
    Int i = 0;
    while ((mapping = VG_(OSetGen_Next)(pmem.pmem_mappings)) != NULL) {
        VG_(umsg)("[%d] Mapping base: 0x%lx\tsize: %llu\n", i++, mapping->addr,
                mapping->size);
    }
}

/**
* \brief Prints gdb monitor commands.
*/
static void
print_monitor_help(void)
{
    VG_(gdb_printf)
            ("\n"
            "pmemcheck gdb monitor commands:\n"
            "  print_stats\n"
            "        prints the summary\n"
            "  print_pmem_regions \n"
            "        prints the registered persistent memory regions\n"
            "  print_log_regions\n"
            "        prints the registered loggable persistent memory regions\n"
            "\n");
}

/**
* \brief Gdb monitor command handler.
* \param[in] tid Id of the calling thread.
* \param[in] req Command request string.
* \return True if command is recognized, true otherwise.
*/
static Bool handle_gdb_monitor_command(ThreadId tid, HChar *req)
{
    HChar* wcmd;
    HChar s[VG_(strlen(req)) + 1]; /* copy for strtok_r */
    HChar *ssaveptr;

    VG_(strcpy) (s, req);

    wcmd = VG_(strtok_r) (s, " ", &ssaveptr);
    switch (VG_(keyword_id)
            ("help print_stats print_pmem_regions print_log_regions",
                    wcmd, kwd_report_duplicated_matches)) {
        case -2: /* multiple matches */
            return True;

        case -1: /* not found */
            return False;

        case  0: /* help */
            print_monitor_help();
            return True;

        case  1:  /* print_stats */
            print_pmem_stats();
            return True;

        case  2: {/* print_pmem_regions */
            VG_(gdb_printf)("Registered persistent memory regions:\n");
            struct pmem_st *tmp = NULL;
            while ((tmp = VG_(OSetGen_Next)(pmem.pmem_stores)) != NULL) {
                VG_(gdb_printf)("\tAddress: 0x%lx \tsize: %llu\n",
                        tmp->addr, tmp->size);
            }
            return True;
        }

        case  3: { /* print_log_regions */
            VG_(gdb_printf)("Registered loggable persistent memory regions:\n");
            struct pmem_st *tmp = NULL;
            while ((tmp = VG_(OSetGen_Next)(pmem.loggable_regions)) != NULL) {
                VG_(gdb_printf)("\tAddress: 0x%lx \tsize: %llu\n",
                        tmp->addr, tmp->size);
            }
            return True;
        }

        default:
            tl_assert(0);
            return False;
    }
}

/**
* \brief The main instrumentation function - the heart of the tool.
*
* The translated client code is passed into this function, where appropriate
* instrumentation is made. All uninteresting operations are copied straight
* to the returned IRSB. The only interesting operations are stores, which are
* instrumented for further analisys.
* \param[in] closure Valgrind closure - unused.
* \param[in] bb The IR superblock provided by the core.
* \param[in] layout Vex quest layout - unused.
* \param[in] vge Vex quest extents - unused.
* \param[in] archinfo_host Vex architecture info - unused.
* \param[in] gWordTy Guest word type.
* \param[in] hWordTy Host word type.
* \return The modified IR superblock.
*/
static IRSB*
pmc_instrument(VgCallbackClosure *closure,
        IRSB *bb,
        VexGuestLayout *layout,
        VexGuestExtents *vge,
        VexArchInfo *archinfo_host,
        IRType gWordTy, IRType hWordTy)
{
    Int i;
    IRSB *sbOut;
    IRTypeEnv *tyenv = bb->tyenv;

    if (gWordTy != hWordTy) {
        /* We don't currently support this case. */
        VG_(tool_panic)("host/guest word size mismatch");
    }

    /* Set up SB */
    sbOut = deepCopyIRSBExceptStmts(bb);

    /* Copy verbatim any IR preamble preceding the first IMark */
    i = 0;
    while (i < bb->stmts_used && bb->stmts[i]->tag != Ist_IMark) {
        addStmtToIRSB(sbOut, bb->stmts[i]);
        ++i;
    }

    /* Count this superblock. */
    IRDirty *di = unsafeIRDirty_0_N( 0, "add_one_SB_entered",
            VG_(fnptr_to_fnentry)(&add_one_SB_entered), mkIRExprVec_0());
    addStmtToIRSB(sbOut, IRStmt_Dirty(di));

    for (/*use current i*/; i < bb->stmts_used; i++) {
        IRStmt *st = bb->stmts[i];
        if (!st || st->tag == Ist_NoOp)
            continue;

        switch (st->tag) {
            case Ist_IMark:
            case Ist_AbiHint:
            case Ist_Put:
            case Ist_PutI:
            case Ist_MBE:
            case Ist_LoadG:
            case Ist_WrTmp:
            case Ist_Exit:
                /* for now we are not interested in any of the above */
                addStmtToIRSB(sbOut, st);
                break;

            case Ist_Store: {
                IRExpr *data = st->Ist.Store.data;
                IRType type = typeOfIRExpr(tyenv, data);
                tl_assert(type != Ity_INVALID);
                add_event_dw(sbOut, st->Ist.Store.addr, sizeofIRType(type),
                        data);
                addStmtToIRSB(sbOut, st);
                break;
            }

            case Ist_StoreG: {
                IRStoreG *sg = st->Ist.StoreG.details;
                IRExpr *data = sg->data;
                IRType type = typeOfIRExpr(tyenv, data);
                tl_assert(type != Ity_INVALID);
                add_event_dw_guarded(sbOut, sg->addr, sizeofIRType(type),
                        sg->guard, data);
                addStmtToIRSB(sbOut, st);
                break;
            }

            case Ist_Dirty: {
                Int dsize;
                IRDirty *d = st->Ist.Dirty.details;
                if (d->mFx != Ifx_None) {
                    /* This dirty helper accesses memory. Collect details. */
                    tl_assert(d->mAddr != NULL);
                    tl_assert(d->mSize != 0);
                    dsize = d->mSize;
                    if (d->mFx == Ifx_Write || d->mFx == Ifx_Modify)
                        add_event_dw_guarded(sbOut, d->mAddr, dsize, d->guard,
                                mkexpr(d->tmp));
                } else {
                    tl_assert(d->mAddr == NULL);
                    tl_assert(d->mSize == 0);
                }
                addStmtToIRSB(sbOut, st);
                break;
            }

            case Ist_CAS: {
                Int dataSize;
                IRType dataTy;
                IRCAS *cas = st->Ist.CAS.details;
                tl_assert(cas->addr != NULL);
                tl_assert(cas->dataLo != NULL);
                dataTy = typeOfIRExpr(tyenv, cas->dataLo);
                dataSize = sizeofIRType(dataTy);
                /* has to be done before registering the guard */
                addStmtToIRSB(sbOut, st);
                /* the guard statement on the CAS */
                IROp opCasCmpEQ;
                IROp opOr;
                IROp opXor;
                IRAtom *zero = NULL;
                IRType loType = typeOfIRExpr(tyenv, cas->expdLo);
                switch (loType) {
                    case Ity_I8:
                        opCasCmpEQ = Iop_CasCmpEQ8;
                        opOr = Iop_Or8;
                        opXor = Iop_Xor8;
                        break;
                    case Ity_I16:
                        opCasCmpEQ = Iop_CasCmpEQ16;
                        opOr = Iop_Or16;
                        opXor = Iop_Xor16;
                        break;
                    case Ity_I32:
                        opCasCmpEQ = Iop_CasCmpEQ32;
                        opOr = Iop_Or32;
                        opXor = Iop_Xor32;
                        break;
                    case Ity_I64:
                        opCasCmpEQ = Iop_CasCmpEQ64;
                        opOr = Iop_Or64;
                        opXor = Iop_Xor64;
                        break;
                    default:
                        tl_assert(0);
                }

                if (cas->dataHi != NULL) {
                    IRAtom *xHi = NULL;
                    IRAtom *xLo = NULL;
                    IRAtom *xHL = NULL;
                    xHi = make_expr(sbOut, loType, binop(opXor, cas->expdHi,
                            mkexpr(cas->oldHi)));
                    xLo = make_expr(sbOut, loType, binop(opXor, cas->expdLo,
                            mkexpr(cas->oldLo)));
                    xHL = make_expr(sbOut, loType, binop(opOr, xHi, xLo));
                    IRAtom *guard = make_expr(sbOut, Ity_I1,
                            binop(opCasCmpEQ, xHL, zero));

                    add_event_dw_guarded(sbOut, cas->addr, dataSize, guard,
                            cas->dataLo);
                    add_event_dw_guarded(sbOut, cas->addr + dataSize,
                            dataSize, guard, cas->dataHi);
                } else {
                    IRAtom *guard = make_expr(sbOut, Ity_I1, binop(opCasCmpEQ,
                            cas->expdLo, mkexpr(cas->oldLo)));

                    add_event_dw_guarded(sbOut, cas->addr, dataSize, guard,
                            cas->dataLo);
                }
                break;
            }

            case Ist_LLSC: {
                IRType dataTy;
                if (st->Ist.LLSC.storedata != NULL) {
                    dataTy = typeOfIRExpr(tyenv, st->Ist.LLSC.storedata);
                    add_event_dw(sbOut, st->Ist.LLSC.addr, sizeofIRType
                            (dataTy), st->Ist.LLSC.storedata);
                }
                addStmtToIRSB(sbOut, st);
                break;
            }

            default:
                ppIRStmt(st);
                tl_assert(0);
        }
    }

    return sbOut;
}

/**
* \brief Client mechanism handler.
* \param[in] tid Id of the calling thread.
* \param[in] arg Arguments passed in the request, 0-th is the request name.
* \param[in,out] ret Return value passed to the client.
* \return True if the request has been handled, false otherwise.
*/
static Bool
pmc_handle_client_request(ThreadId tid, UWord *arg, UWord *ret )
{
    if (!VG_IS_TOOL_USERREQ('P', 'C', arg[0])
            && VG_USERREQ__PMC_REGISTER_PMEM_MAPPING != arg[0]
            && VG_USERREQ__PMC_REGISTER_PMEM_FILE != arg[0]
            && VG_USERREQ__PMC_REMOVE_PMEM_MAPPING != arg[0]
            && VG_USERREQ__PMC_CHECK_IS_PMEM_MAPPING != arg[0]
            && VG_USERREQ__PMC_DO_FLUSH != arg[0]
            && VG_USERREQ__PMC_DO_FENCE != arg[0]
            && VG_USERREQ__PMC_DO_COMMIT != arg[0]
            && VG_USERREQ__PMC_WRITE_STATS != arg[0]
            && VG_USERREQ__GDB_MONITOR_COMMAND != arg[0]
            && VG_USERREQ__PMC_PRINT_PMEM_MAPPINGS != arg[0]
            && VG_USERREQ__PMC_LOG_STORES != arg[0]
            && VG_USERREQ__PMC_NO_LOG_STORES != arg[0]
            && VG_USERREQ__PMC_ADD_LOG_REGION != arg[0]
            && VG_USERREQ__PMC_REMOVE_LOG_REGION != arg[0]
            && VG_USERREQ__PMC_FULL_REORDED != arg[0]
            && VG_USERREQ__PMC_PARTIAL_REORDER != arg[0]
            && VG_USERREQ__PMC_ONLY_FAULT != arg[0]
            && VG_USERREQ__PMC_STOP_REORDER_FAULT != arg[0]
            )
        return False;

    switch (arg[0]) {
        case VG_USERREQ__PMC_REGISTER_PMEM_MAPPING: {
            struct pmem_st temp_info;
            temp_info.addr = arg[1];
            temp_info.size = arg[2];

            add_region(&temp_info, pmem.pmem_mappings);
            *ret = 1;
            break;
        }

        case VG_USERREQ__PMC_REMOVE_PMEM_MAPPING: {
            struct pmem_st temp_info;
            temp_info.addr = arg[1];
            temp_info.size = arg[2];

            remove_region(&temp_info, pmem.pmem_mappings);
            *ret = 1;
            break;
        }

        case VG_USERREQ__PMC_REGISTER_PMEM_FILE: {
            *ret = 0;
            Int fd = (Int)arg[1];
            if (fd >= 0)
                *ret = register_new_file(fd, arg[2], arg[3], arg[4]);
         break;
        }

        case VG_USERREQ__PMC_CHECK_IS_PMEM_MAPPING: {
            struct pmem_st temp_info;
            temp_info.addr = arg[1];
            temp_info.size = arg[2];

            *ret = is_in_mapping_set(&temp_info, pmem.pmem_mappings);
            break;
        }

        case VG_USERREQ__PMC_PRINT_PMEM_MAPPINGS: {
            print_persistent_mappings();
            break;
        }

        case VG_USERREQ__PMC_DO_FLUSH: {
            do_flush(arg[1], arg[2]);
            *ret = 1;
            break;
        }

        case VG_USERREQ__PMC_DO_FENCE: {
            do_fence();
            *ret = 1;
            break;
        }

        case VG_USERREQ__PMC_DO_COMMIT: {
            do_commit();
            *ret = 1;
            break;
        }

        case VG_USERREQ__PMC_WRITE_STATS: {
            print_pmem_stats();
            *ret = 1;
            break;
        }

        case VG_USERREQ__GDB_MONITOR_COMMAND: {
            Bool handled = handle_gdb_monitor_command (tid, (HChar*)arg[1]);
            if (handled)
                *ret = 1;
            else
                *ret = 0;
            return handled;
        }

        case VG_USERREQ__PMC_LOG_STORES: {
            pmem.loggin_on = True;
            *ret = 1;
            break;
        }

        case VG_USERREQ__PMC_NO_LOG_STORES: {
            pmem.loggin_on = False;
            *ret = 1;
            break;
        }

        case VG_USERREQ__PMC_ADD_LOG_REGION: {
            struct pmem_st temp_info;
            temp_info.addr = arg[1];
            temp_info.size = arg[2];

            add_region(&temp_info, pmem.loggable_regions);
            *ret = 1;
            break;
        }

        case VG_USERREQ__PMC_REMOVE_LOG_REGION: {
            struct pmem_st temp_info;
            temp_info.addr = arg[1];
            temp_info.size = arg[2];

            remove_region(&temp_info, pmem.loggable_regions);
            *ret = 1;
            break;
        }

        case VG_USERREQ__PMC_FULL_REORDED: {
            if (pmem.log_stores && (pmem.loggin_on || (VG_(OSetGen_Size)
                    (pmem.loggable_regions) != 0)))
                VG_(emit)("|FREORDER");
            *ret = 1;
            break;
        }

        case VG_USERREQ__PMC_PARTIAL_REORDER: {
            if (pmem.log_stores && (pmem.loggin_on || (VG_(OSetGen_Size)
                    (pmem.loggable_regions) != 0)))
                VG_(emit)("|PREORDER");
            *ret = 1;
            break;
        }

        case VG_USERREQ__PMC_ONLY_FAULT: {
            if (pmem.log_stores && (pmem.loggin_on || (VG_(OSetGen_Size)
                    (pmem.loggable_regions) != 0)))
                VG_(emit)("|FAULT_ONLY");
            *ret = 1;
            break;
        }

        case VG_USERREQ__PMC_STOP_REORDER_FAULT: {
            if (pmem.log_stores && (pmem.loggin_on || (VG_(OSetGen_Size)
                    (pmem.loggable_regions) != 0)))
                VG_(emit)("|NO_REORDER_FAULT");
            *ret = 1;
            break;
        }

        default:
            VG_(message)(
                    Vg_UserMsg,
                    "Warning: unknown pmemcheck client request code 0x%llx\n",
                    (ULong)arg[0]
            );
            return False;
    }
    return True;
}

/**
* \brief Handle tool command line arguments.
* \param[in] arg Tool command line arguments.
* \return True if the parameter is recognized, false otherwise.
*/
static Bool
pmc_process_cmd_line_option(const HChar *arg)
{
    if VG_BOOL_CLO(arg, "--mult-stores", pmem.track_multiple_stores) {}
    else if VG_BINT_CLO(arg, "--indiff", pmem.store_sb_indiff, 0, UINT_MAX) {}
    else if VG_BOOL_CLO(arg, "--log-stores", pmem.log_stores) {}
    else if VG_BOOL_CLO(arg, "--print-summary", pmem.print_summary) {}
    else if VG_BOOL_CLO(arg, "--flush-check", pmem.check_flush) {}
    else
        return False;

    return True;
}

/**
* \brief Post command line options initialization.
*/
static void
pmc_post_clo_init(void)
{
    pmem.pmem_stores = VG_(OSetGen_Create)(/*keyOff*/0, cmp_pmem_st,
            VG_(malloc), "pmc.main.cpci.1", VG_(free));

    if (pmem.track_multiple_stores)
        pmem.multiple_stores = VG_(malloc)("pmc.main.cpci.2",
                MAX_MULT_OVERWRITES * sizeof (struct pmem_st *));

    pmem.flush_errors = VG_(malloc)("pmc.main.cpci.3", MAX_FLUSH_ERROR_EVENTS
            * sizeof (struct pmem_st *));

    pmem.pmem_mappings = VG_(OSetGen_Create)(/*keyOff*/0, cmp_pmem_st,
            VG_(malloc), "pmc.main.cpci.4", VG_(free));

    pmem.loggable_regions = VG_(OSetGen_Create)(/*keyOff*/0, cmp_pmem_st,
            VG_(malloc), "pmc.main.cpci.5", VG_(free));

    pmem.flush_align = read_cache_line_size();

    if(pmem.log_stores)
        VG_(umsg)("START");
}

/**
* \brief Print usage.
*/
static void
pmc_print_usage(void)
{
    VG_(printf)(
            "    --indiff=<uint>            multiple store indifference\n"
            "                               default [0 SBlocks]\n"
            "    --mult-stores=<yes|no>     track multiple stores to the same\n"
            "                               address default [no]\n"
            "    --log-stores=<yes|no>      log all stores to persistence\n"
            "                               default [no]\n"
            "    --print-summary=<yes|no>   print summary on program exit\n"
            "                               default [yes]\n"
            "    --flush-check=<yes|no>     register multiple flushes of stores\n"
            "                               default [no]\n"
    );
}

/**
* \brief Print debug usage.
*/
static void
pmc_print_debug_usage(void)
{
    VG_(printf)(
            "    (none)\n"
    );
}

/**
 * \brief Function called on program exit.
 */
static void
pmc_fini(Int exitcode)
{
    if(pmem.log_stores)
        VG_(umsg)("|STOP\n");

    if (pmem.print_summary)
        print_pmem_stats();
}

/**
* \brief Pre command line options initialization.
*/
static void
pmc_pre_clo_init(void)
{
    VG_(details_name)("pmemcheck");
    VG_(details_version)("0.1");
    VG_(details_description)("a simple persistent store checker");
    VG_(details_copyright_author)("Copyright (c) 2014-2015, Intel Corporation");
    VG_(details_bug_reports_to)("tomasz.kapela@intel.com");

    VG_(details_avg_translation_sizeB)(275);

    VG_(basic_tool_funcs)(pmc_post_clo_init, pmc_instrument, pmc_fini);

    VG_(needs_command_line_options)(pmc_process_cmd_line_option,
            pmc_print_usage, pmc_print_debug_usage);

    VG_(needs_client_requests)(pmc_handle_client_request);

    /* support only 64 bit architectures */
    tl_assert(VG_WORDSIZE == 8);
    tl_assert(sizeof(void*) == 8);
    tl_assert(sizeof(Addr) == 8);
    tl_assert(sizeof(UWord) == 8);
    tl_assert(sizeof(Word) == 8);

    pmem.print_summary = True;
}

VG_DETERMINE_INTERFACE_VERSION(pmc_pre_clo_init)