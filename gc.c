/**********************************************************************

  gc.c -

  $Author$
  created at: Tue Oct  5 09:44:46 JST 1993

  Copyright (C) 1993-2007 Yukihiro Matsumoto
  Copyright (C) 2000  Network Applied Communication Laboratory, Inc.
  Copyright (C) 2000  Information-technology Promotion Agency, Japan

**********************************************************************/

#include "ruby/ruby.h"
#include "ruby/st.h"
#include "ruby/re.h"
#include "ruby/io.h"
#include "ruby/util.h"
#include "eval_intern.h"
#include "vm_core.h"
#include "internal.h"
#include "gc.h"
#include "pool_alloc.h"
#include "constant.h"
#include <stdio.h>
#include <setjmp.h>
#include <sys/types.h>
#include <assert.h>

#include "timing.c"

#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif

#if defined _WIN32 || defined __CYGWIN__
#include <windows.h>
#elif defined(HAVE_POSIX_MEMALIGN)
#elif defined(HAVE_MEMALIGN)
#include <malloc.h>
#endif
static void aligned_free(void *);
static void *aligned_malloc(size_t alignment, size_t size);

#ifdef HAVE_VALGRIND_MEMCHECK_H
# include <valgrind/memcheck.h>
# ifndef VALGRIND_MAKE_MEM_DEFINED
#  define VALGRIND_MAKE_MEM_DEFINED(p, n) VALGRIND_MAKE_READABLE((p), (n))
# endif
# ifndef VALGRIND_MAKE_MEM_UNDEFINED
#  define VALGRIND_MAKE_MEM_UNDEFINED(p, n) VALGRIND_MAKE_WRITABLE((p), (n))
# endif
#else
# define VALGRIND_MAKE_MEM_DEFINED(p, n) /* empty */
# define VALGRIND_MAKE_MEM_UNDEFINED(p, n) /* empty */
#endif

#define rb_setjmp(env) RUBY_SETJMP(env)
#define rb_jmp_buf rb_jmpbuf_t

/* Make alloca work the best possible way.  */
#ifdef __GNUC__
# ifndef atarist
#  ifndef alloca
#   define alloca __builtin_alloca
#  endif
# endif /* atarist */
#else
# ifdef HAVE_ALLOCA_H
#  include <alloca.h>
# else
#  ifdef _AIX
 #pragma alloca
#  else
#   ifndef alloca /* predefined by HP cc +Olibcalls */
void *alloca ();
#   endif
#  endif /* AIX */
# endif /* HAVE_ALLOCA_H */
#endif /* __GNUC__ */

#ifndef GC_MALLOC_LIMIT
#define GC_MALLOC_LIMIT 8000000
#endif
#define HEAP_MIN_SLOTS 10000
#define FREE_MIN  4096
#define HEAP_GROWTH_FACTOR 1.8

typedef struct {
    unsigned int initial_malloc_limit;
    unsigned int initial_heap_min_slots;
    unsigned int initial_free_min;
    double initial_growth_factor;
#if defined(ENABLE_VM_OBJSPACE) && ENABLE_VM_OBJSPACE
    int gc_stress;
#endif
    int track_metadata;
} ruby_gc_params_t;

static ruby_gc_params_t initial_params = {
    GC_MALLOC_LIMIT,
    HEAP_MIN_SLOTS,
    FREE_MIN,
    HEAP_GROWTH_FACTOR,
#if defined(ENABLE_VM_OBJSPACE) && ENABLE_VM_OBJSPACE
    FALSE,
#endif
    FALSE
};

#define nomem_error GET_VM()->special_exceptions[ruby_error_nomemory]

#define MARK_STACK_MAX 1024

int ruby_gc_debug_indent = 0;

/* for GC profile */
#ifndef GC_PROFILE_MORE_DETAIL
#define GC_PROFILE_MORE_DETAIL 0
#endif

typedef struct gc_profile_record {
    double gc_time;
    double gc_mark_time;
    double gc_sweep_time;
    double gc_invoke_time;

    size_t heap_use_slots;
    size_t heap_live_objects;
    size_t heap_free_objects;
    size_t heap_total_objects;
    size_t heap_use_size;
    size_t heap_total_size;

    int have_finalize;
    int is_marked;

    size_t allocate_increase;
    size_t allocate_limit;
} gc_profile_record;

#define GC_PROF_TIMER_START do {\
	if (objspace->profile.run) {\
	    if (!objspace->profile.record) {\
		objspace->profile.size = 1000;\
		objspace->profile.record = malloc(sizeof(gc_profile_record) * objspace->profile.size);\
	    }\
	    if (count >= objspace->profile.size) {\
		objspace->profile.size += 1000;\
		objspace->profile.record = realloc(objspace->profile.record, sizeof(gc_profile_record) * objspace->profile.size);\
	    }\
	    if (!objspace->profile.record) {\
		rb_bug("gc_profile malloc or realloc miss");\
	    }\
	    MEMZERO(&objspace->profile.record[count], gc_profile_record, 1);\
	    gc_time = getrusage_time();\
	    objspace->profile.record[count].gc_invoke_time = gc_time - objspace->profile.invoke_time;\
	}\
	if (objspace->basic_profile.run) {\
	  objspace->basic_profile.gc_start = getrusage_time();\
	}\
    } while(0)

#define GC_PROF_TIMER_STOP(marked) do {\
	if (objspace->profile.run) {\
	    gc_time = getrusage_time() - gc_time;\
	    if (gc_time < 0) gc_time = 0;\
	    objspace->gc_time += gc_time;\
	    objspace->profile.record[count].gc_time = gc_time;\
	    objspace->profile.record[count].is_marked = !!(marked);\
	    GC_PROF_SET_HEAP_INFO(objspace->profile.record[count]);\
	    objspace->profile.count++;\
	}\
	if (objspace->basic_profile.run) {\
	  objspace->basic_profile.total_time += getrusage_time() - objspace->basic_profile.gc_start;\
	  objspace->basic_profile.gc_start = 0;\
	  objspace->basic_profile.count++;\
	}\
    } while(0)

#if GC_PROFILE_MORE_DETAIL
#define INIT_GC_PROF_PARAMS double gc_time = 0, sweep_time = 0;\
    size_t count = objspace->profile.count, total = 0, live = 0

#define GC_PROF_MARK_TIMER_START double mark_time = 0;\
    do {\
	if (objspace->profile.run) {\
	    mark_time = getrusage_time();\
	}\
    } while(0)

#define GC_PROF_MARK_TIMER_STOP do {\
	if (objspace->profile.run) {\
	    mark_time = getrusage_time() - mark_time;\
	    if (mark_time < 0) mark_time = 0;\
	    objspace->profile.record[objspace->profile.count].gc_mark_time = mark_time;\
	}\
    } while(0)

#define GC_PROF_SWEEP_TIMER_START do {\
	if (objspace->profile.run) {\
	    sweep_time = getrusage_time();\
	}\
    } while(0)

#define GC_PROF_SWEEP_TIMER_STOP do {\
	if (objspace->profile.run) {\
	    sweep_time = getrusage_time() - sweep_time;\
	    if (sweep_time < 0) sweep_time = 0;\
	    objspace->profile.record[count].gc_sweep_time = sweep_time;\
	}\
    } while(0)
#define GC_PROF_SET_MALLOC_INFO do {\
	if (objspace->profile.run) {\
	    gc_profile_record *record = &objspace->profile.record[objspace->profile.count];\
	    record->allocate_increase = malloc_increase;\
	    record->allocate_limit = malloc_limit; \
	}\
    } while(0)
#define GC_PROF_SET_HEAP_INFO(record) do {\
        live = objspace_live_num(objspace);\
        total = heaps_used * HEAP_OBJ_LIMIT;\
        (record).heap_use_slots = heaps_used;\
        (record).heap_live_objects = live;\
        (record).heap_free_objects = total - live;\
        (record).heap_total_objects = total;\
        (record).have_finalize = deferred_final_list ? Qtrue : Qfalse;\
        (record).heap_use_size = live * sizeof(RVALUE);\
        (record).heap_total_size = total * sizeof(RVALUE);\
    } while(0)
#else
#define INIT_GC_PROF_PARAMS double gc_time = 0;\
    size_t count = objspace->profile.count, total = 0, live = 0
#define GC_PROF_MARK_TIMER_START
#define GC_PROF_MARK_TIMER_STOP
#define GC_PROF_SWEEP_TIMER_START
#define GC_PROF_SWEEP_TIMER_STOP
#define GC_PROF_SET_MALLOC_INFO
#define GC_PROF_SET_HEAP_INFO(record) do {\
        live = objspace_live_num(objspace);\
        total = heaps_used * HEAP_OBJ_LIMIT;\
        (record).heap_total_objects = total;\
        (record).heap_use_size = live * sizeof(RVALUE);\
        (record).heap_total_size = total * sizeof(RVALUE);\
    } while(0)
#endif

#ifndef INT2BOOL
#define INT2BOOL(x)  ((x)?Qtrue:Qfalse)
#endif

#if defined(_MSC_VER) || defined(__BORLANDC__) || defined(__CYGWIN__)
#pragma pack(push, 1) /* magic for reducing sizeof(RVALUE): 24 -> 20 */
#endif

typedef struct RVALUE {
    union {
	struct {
	    VALUE flags;		/* always 0 for freed obj */
	    struct RVALUE *next;
	} free;
	struct RBasic  basic;
	struct RObject object;
	struct RClass  klass;
	struct RFloat  flonum;
	struct RString string;
	struct RArray  array;
	struct RRegexp regexp;
	struct RHash   hash;
	struct RData   data;
	struct RTypedData   typeddata;
	struct RStruct rstruct;
	struct RBignum bignum;
	struct RFile   file;
	struct RNode   node;
	struct RMatch  match;
	struct RRational rational;
	struct RComplex complex;
    } as;
} RVALUE;

#if defined(_MSC_VER) || defined(__BORLANDC__) || defined(__CYGWIN__)
#pragma pack(pop)
#endif

typedef struct rb_obj_metadata {
    VALUE file;
    unsigned short line;
} rb_obj_metadata_t;

struct heaps_slot {
    struct heaps_header *membase;
    RVALUE *freelist;
    struct heaps_slot *next;
    struct heaps_slot *prev;
    struct heaps_slot *free_next;
    uintptr_t bits[1];
};

struct heaps_header {
    struct heaps_slot *base;
    uintptr_t *bits;
    RVALUE *start;
    RVALUE *end;
    size_t limit;
    rb_obj_metadata_t *metadata;
};

struct gc_list {
    VALUE *varptr;
    struct gc_list *next;
};

#ifndef CALC_EXACT_MALLOC_SIZE
#define CALC_EXACT_MALLOC_SIZE 0
#endif

#ifdef POOL_ALLOC_API
/* POOL ALLOC API */
#define POOL_ALLOC_PART 1
#include "pool_alloc.inc.h"
#undef POOL_ALLOC_PART

typedef struct pool_layout_t pool_layout_t;
struct pool_layout_t {
    pool_header
      p6,  /* st_table && st_table_entry */
      p11;  /* st_table.bins init size */
} pool_layout = {
    INIT_POOL(void*[6]),
    INIT_POOL(void*[11])
};
static void pool_finalize_header(pool_header *header);
#endif

typedef struct rb_gc_basic_profile {
  int run;
  double gc_start;
  double total_time;
  size_t count;
} rb_gc_basic_profile_t;

typedef struct rb_objspace {
    struct {
	size_t limit;
	size_t increase;
#if CALC_EXACT_MALLOC_SIZE
	size_t allocated_size;
	size_t allocations;
#endif
    } malloc_params;
#ifdef POOL_ALLOC_API
    pool_layout_t *pool_headers;
#endif
    struct {
	size_t increment;
	size_t decrement;
	struct heaps_slot *ptr;
	struct heaps_slot *sweep_slots;
	struct heaps_slot *free_slots;
	struct heaps_header **sorted;
	size_t length;
	size_t used;
        struct heaps_slot *reserve_slots;
	RVALUE *range[2];
	size_t marked_num;
	size_t swept_num;
	size_t final_num;
	size_t free_min;
	size_t free_max;
    } heap;
    struct {
	int dont_gc;
	int dont_lazy_sweep;
	int during_gc;
	rb_atomic_t finalizing;
    } flags;
    struct {
	st_table *table;
	RVALUE *deferred;
    } final;
    struct {
	VALUE buffer[MARK_STACK_MAX];
	VALUE *ptr;
	int overflow;
    } markstack;
    struct {
	int run;
	gc_profile_record *record;
	size_t count;
	size_t size;
	double invoke_time;
    } profile;
    struct rb_gc_basic_profile basic_profile;
    struct gc_list *global_list;
    size_t count;
    double gc_time;
    size_t total_allocated_object_num;
    size_t total_freed_object_num;
    int gc_stress;
} rb_objspace_t;

#if defined(ENABLE_VM_OBJSPACE) && ENABLE_VM_OBJSPACE
#define rb_objspace (*GET_VM()->objspace)
#define ruby_initial_gc_stress	initial_params.gc_stress
int *ruby_initial_gc_stress_ptr = &ruby_initial_gc_stress;
#else
#  ifdef POOL_ALLOC_API
static rb_objspace_t rb_objspace = {{GC_MALLOC_LIMIT}, &pool_layout, {HEAP_MIN_SLOTS}};
#  else
static rb_objspace_t rb_objspace = {{GC_MALLOC_LIMIT}, {HEAP_MIN_SLOTS}};
#  endif
int *ruby_initial_gc_stress_ptr = &rb_objspace.gc_stress;
#endif
#define malloc_limit		objspace->malloc_params.limit
#define malloc_increase 	objspace->malloc_params.increase
#define heaps			objspace->heap.ptr
#define heaps_length		objspace->heap.length
#define heaps_used		objspace->heap.used
#define lomem			objspace->heap.range[0]
#define himem			objspace->heap.range[1]
#define heaps_inc		objspace->heap.increment
#define heaps_dec		objspace->heap.decrement
#define dont_gc 		objspace->flags.dont_gc
#define during_gc		objspace->flags.during_gc
#define finalizing		objspace->flags.finalizing
#define finalizer_table 	objspace->final.table
#define deferred_final_list	objspace->final.deferred
#define mark_stack		objspace->markstack.buffer
#define mark_stack_ptr		objspace->markstack.ptr
#define mark_stack_overflow	objspace->markstack.overflow
#define global_List		objspace->global_list
#define ruby_gc_stress		objspace->gc_stress
#define initial_malloc_limit	initial_params.initial_malloc_limit
#define initial_heap_min_slots	initial_params.initial_heap_min_slots
#define initial_free_min	initial_params.initial_free_min
#define initial_growth_factor	initial_params.initial_growth_factor
#define track_metadata		initial_params.track_metadata

#define is_lazy_sweeping(objspace) ((objspace)->heap.sweep_slots != 0)

#define nonspecial_obj_id(obj) (VALUE)((SIGNED_VALUE)(obj)|FIXNUM_FLAG)

#define HEAP_HEADER(p) ((struct heaps_header *)(p))

static void rb_objspace_call_finalizer(rb_objspace_t *objspace);
static VALUE define_final0(VALUE obj, VALUE block);
VALUE rb_define_final(VALUE obj, VALUE block);
VALUE rb_undefine_final(VALUE obj);

#if defined(ENABLE_VM_OBJSPACE) && ENABLE_VM_OBJSPACE
rb_objspace_t *
rb_objspace_alloc(void)
{
    rb_objspace_t *objspace = malloc(sizeof(rb_objspace_t));
    memset(objspace, 0, sizeof(*objspace));
    malloc_limit = initial_malloc_limit;
    ruby_gc_stress = ruby_initial_gc_stress;
#ifdef POOL_ALLOC_API
    objspace->pool_headers = (pool_layout_t*) malloc(sizeof(pool_layout));
    memcpy(objspace->pool_headers, &pool_layout, sizeof(pool_layout));
#endif

    return objspace;
}
#endif

static void initial_expand_heap(rb_objspace_t *objspace);

void
rb_obj_enable_metadata(void)
{
    track_metadata = TRUE;
}

void
rb_gc_set_params(void)
{
    char *malloc_limit_ptr, *heap_min_slots_ptr, *free_min_ptr, *growth_factor_ptr;

    if (rb_safe_level() > 0) return;

    malloc_limit_ptr = getenv("RUBY_GC_MALLOC_LIMIT");
    if (malloc_limit_ptr != NULL) {
	int malloc_limit_i = atoi(malloc_limit_ptr);
	if (RTEST(ruby_verbose))
	    fprintf(stderr, "malloc_limit=%d (%d)\n",
		    malloc_limit_i, initial_malloc_limit);
	if (malloc_limit_i > 0) {
	    initial_malloc_limit = malloc_limit_i;
	}
    }

    heap_min_slots_ptr = getenv("RUBY_HEAP_MIN_SLOTS");
    if (heap_min_slots_ptr != NULL) {
	int heap_min_slots_i = atoi(heap_min_slots_ptr);
	if (RTEST(ruby_verbose))
	    fprintf(stderr, "heap_min_slots=%d (%d)\n",
		    heap_min_slots_i, initial_heap_min_slots);
	if (heap_min_slots_i > 0) {
	    initial_heap_min_slots = heap_min_slots_i;
            initial_expand_heap(&rb_objspace);
	}
    }

    growth_factor_ptr = getenv("RUBY_HEAP_SLOTS_GROWTH_FACTOR");
    if (growth_factor_ptr != NULL) {
	double growth_factor_f = atof(growth_factor_ptr);
	if (RTEST(ruby_verbose))
	    fprintf(stderr, "growth_factor=%f (%f)\n", growth_factor_f, initial_growth_factor);
	if (growth_factor_f > 0) {
	    initial_growth_factor = growth_factor_f;
	}
    }

    free_min_ptr = getenv("RUBY_FREE_MIN");
    if (free_min_ptr != NULL) {
	int free_min_i = atoi(free_min_ptr);
	if (RTEST(ruby_verbose))
	    fprintf(stderr, "free_min=%d (%d)\n", free_min_i, initial_free_min);
	if (free_min_i > 0) {
	    initial_free_min = free_min_i;
	}
    }
}

#if defined(ENABLE_VM_OBJSPACE) && ENABLE_VM_OBJSPACE
static void gc_sweep(rb_objspace_t *);
static void slot_sweep(rb_objspace_t *, struct heaps_slot *);
static void rest_sweep(rb_objspace_t *);

void
rb_objspace_free(rb_objspace_t *objspace)
{
    rest_sweep(objspace);
    if (objspace->profile.record) {
	free(objspace->profile.record);
	objspace->profile.record = 0;
    }
    if (global_List) {
	struct gc_list *list, *next;
	for (list = global_List; list; list = next) {
	    next = list->next;
	    xfree(list);
	}
    }
    if (objspace->heap.reserve_slots) {
        struct heaps_slot *list, *next;
        for (list = objspace->heap.reserve_slots; list; list = next) {
            next = list->free_next;
            free(list);
        }
    }
    if (objspace->heap.sorted) {
	size_t i;
	for (i = 0; i < heaps_used; ++i) {
            free(objspace->heap.sorted[i]->base);
            if (objspace->heap.sorted[i]->metadata)
                free(objspace->heap.sorted[i]->metadata);
	    aligned_free(objspace->heap.sorted[i]);
	}
	free(objspace->heap.sorted);
	heaps_used = 0;
	heaps = 0;
    }
#ifdef POOL_ALLOC_API
    if (objspace->pool_headers) {
        pool_finalize_header(&objspace->pool_headers->p6);
        pool_finalize_header(&objspace->pool_headers->p11);
        free(objspace->pool_headers);
    }
#endif
    free(objspace);
}
#endif

#ifndef HEAP_ALIGN_LOG
/* default tiny heap size: 16KB */
#define HEAP_ALIGN_LOG 14
#endif
#define HEAP_ALIGN (1UL << HEAP_ALIGN_LOG)
#define HEAP_ALIGN_MASK (~(~0UL << HEAP_ALIGN_LOG))
#define REQUIRED_SIZE_BY_MALLOC (sizeof(size_t) * 5)
#define HEAP_SIZE (HEAP_ALIGN - REQUIRED_SIZE_BY_MALLOC)
#define CEILDIV(i, mod) (((i) + (mod) - 1)/(mod))

#define HEAP_OBJ_LIMIT (unsigned int)((HEAP_SIZE - sizeof(struct heaps_header))/sizeof(struct RVALUE))
#define HEAP_BITMAP_LIMIT CEILDIV(CEILDIV(HEAP_SIZE, sizeof(struct RVALUE)), sizeof(uintptr_t)*8)
#define HEAP_SLOT_SIZE (sizeof(struct heaps_slot) + (HEAP_BITMAP_LIMIT-1) * sizeof(uintptr_t))

#define GET_HEAP_HEADER(x) (HEAP_HEADER(((uintptr_t)x) & ~(HEAP_ALIGN_MASK)))
#define GET_HEAP_SLOT(x) (GET_HEAP_HEADER(x)->base)
#define GET_HEAP_BITMAP(x) (GET_HEAP_HEADER(x)->bits)
#define NUM_IN_SLOT(p) (((uintptr_t)p & HEAP_ALIGN_MASK)/sizeof(RVALUE))
#define BITMAP_INDEX(p) (NUM_IN_SLOT(p) / (sizeof(uintptr_t) * 8))
#define BITMAP_OFFSET(p) (NUM_IN_SLOT(p) & ((sizeof(uintptr_t) * 8)-1))
#define MARKED_IN_BITMAP(bits, p) (bits[BITMAP_INDEX(p)] & ((uintptr_t)1 << BITMAP_OFFSET(p)))
#define MARK_IN_BITMAP(bits, p) (bits[BITMAP_INDEX(p)] = bits[BITMAP_INDEX(p)] | ((uintptr_t)1 << BITMAP_OFFSET(p)))
#define CLEAR_IN_BITMAP(bits, p) (bits[BITMAP_INDEX(p)] &= ~((uintptr_t)1 << BITMAP_OFFSET(p)))

extern st_table *rb_class_tbl;

int ruby_disable_gc_stress = 0;

static void run_final(rb_objspace_t *objspace, VALUE obj);
static int garbage_collect(rb_objspace_t *objspace, int mark_only);
static int gc_lazy_sweep(rb_objspace_t *objspace);

void
rb_global_variable(VALUE *var)
{
    rb_gc_register_address(var);
}

static void *
ruby_memerror_body(void *dummy)
{
    rb_memerror();
    return 0;
}

static void
ruby_memerror(void)
{
    if (ruby_thread_has_gvl_p()) {
	rb_memerror();
    }
    else {
	if (ruby_native_thread_p()) {
	    rb_thread_call_with_gvl(ruby_memerror_body, 0);
	}
	else {
	    /* no ruby thread */
	    fprintf(stderr, "[FATAL] failed to allocate memory\n");
	    exit(EXIT_FAILURE);
	}
    }
}

void
rb_memerror(void)
{
    rb_thread_t *th = GET_THREAD();
    if (!nomem_error ||
	(rb_thread_raised_p(th, RAISED_NOMEMORY) && rb_safe_level() < 4)) {
	fprintf(stderr, "[FATAL] failed to allocate memory\n");
	exit(EXIT_FAILURE);
    }
    if (rb_thread_raised_p(th, RAISED_NOMEMORY)) {
	rb_thread_raised_clear(th);
	GET_THREAD()->errinfo = nomem_error;
	JUMP_TAG(TAG_RAISE);
    }
    rb_thread_raised_set(th, RAISED_NOMEMORY);
    rb_exc_raise(nomem_error);
}

/*
 *  call-seq:
 *    GC.stress                 -> true or false
 *
 *  returns current status of GC stress mode.
 */

static VALUE
gc_stress_get(VALUE self)
{
    rb_objspace_t *objspace = &rb_objspace;
    return ruby_gc_stress ? Qtrue : Qfalse;
}

/*
 *  call-seq:
 *    GC.stress = bool          -> bool
 *
 *  Updates the GC stress mode.
 *
 *  When stress mode is enabled the GC is invoked at every GC opportunity:
 *  all memory and object allocations.
 *
 *  Enabling stress mode makes Ruby very slow, it is only for debugging.
 */

static VALUE
gc_stress_set(VALUE self, VALUE flag)
{
    rb_objspace_t *objspace = &rb_objspace;
    rb_secure(2);
    ruby_gc_stress = RTEST(flag);
    return flag;
}

/*
 *  call-seq:
 *    GC::Profiler.enable?                 -> true or false
 *
 *  The current status of GC profile mode.
 */

static VALUE
gc_profile_enable_get(VALUE self)
{
    rb_objspace_t *objspace = &rb_objspace;
    return INT2BOOL(objspace->profile.run);
}

/*
 *  call-seq:
 *    GC::Profiler.enable          -> nil
 *
 *  Starts the GC profiler.
 *
 */

static VALUE
gc_profile_enable(void)
{
    rb_objspace_t *objspace = &rb_objspace;

    objspace->profile.run = TRUE;
    return Qnil;
}

/*
 *  call-seq:
 *    GC::Profiler.disable          -> nil
 *
 *  Stops the GC profiler.
 *
 */

static VALUE
gc_profile_disable(void)
{
    rb_objspace_t *objspace = &rb_objspace;
    objspace->profile.run = FALSE;

    return Qnil;
}

/*
 *  call-seq:
 *    GC::Profiler.clear          -> nil
 *
 *  Clears the GC profiler data.
 *
 */

static VALUE
gc_profile_clear(void)
{
    rb_objspace_t *objspace = &rb_objspace;
    MEMZERO(objspace->profile.record, gc_profile_record, objspace->profile.size);
    objspace->profile.count = 0;
    objspace->gc_time = 0;
    return Qnil;
}

static void *
negative_size_allocation_error_with_gvl(void *ptr)
{
    rb_raise(rb_eNoMemError, "%s", (const char *)ptr);
    return 0; /* should not be reached */
}

static void
negative_size_allocation_error(const char *msg)
{
    if (ruby_thread_has_gvl_p()) {
	rb_raise(rb_eNoMemError, "%s", msg);
    }
    else {
	if (ruby_native_thread_p()) {
	    rb_thread_call_with_gvl(negative_size_allocation_error_with_gvl, (void *)msg);
	}
	else {
	    fprintf(stderr, "[FATAL] %s\n", msg);
	    exit(EXIT_FAILURE);
	}
    }
}

static void *
gc_with_gvl(void *ptr)
{
    return (void *)(VALUE)garbage_collect((rb_objspace_t *)ptr, 0);
}

static int
garbage_collect_with_gvl(rb_objspace_t *objspace)
{
    if (dont_gc) return TRUE;
    if (ruby_thread_has_gvl_p()) {
	return garbage_collect(objspace, 0);
    }
    else {
	if (ruby_native_thread_p()) {
	    return (int)(VALUE)rb_thread_call_with_gvl(gc_with_gvl, (void *)objspace);
	}
	else {
	    /* no ruby thread */
	    fprintf(stderr, "[FATAL] failed to allocate memory\n");
	    exit(EXIT_FAILURE);
	}
    }
}

static void vm_xfree(rb_objspace_t *objspace, void *ptr);

static inline size_t
vm_malloc_prepare(rb_objspace_t *objspace, size_t size)
{
    if ((ssize_t)size < 0) {
	negative_size_allocation_error("negative allocation size (or too big)");
    }
    if (size == 0) size = 1;

#if CALC_EXACT_MALLOC_SIZE
    size += sizeof(size_t);
#endif

    if ((ruby_gc_stress && !ruby_disable_gc_stress) ||
	(malloc_increase+size) > malloc_limit) {
	garbage_collect_with_gvl(objspace);
    }

    return size;
}

static inline void *
vm_malloc_fixup(rb_objspace_t *objspace, void *mem, size_t size)
{
    malloc_increase += size;

#if CALC_EXACT_MALLOC_SIZE
    objspace->malloc_params.allocated_size += size;
    objspace->malloc_params.allocations++;
    ((size_t *)mem)[0] = size;
    mem = (size_t *)mem + 1;
#endif

    return mem;
}

#define TRY_WITH_GC(alloc) do { \
	if (!(alloc) && \
	    (!garbage_collect_with_gvl(objspace) || \
	     !(alloc))) { \
	    ruby_memerror(); \
	} \
    } while (0)

static void *
vm_xmalloc(rb_objspace_t *objspace, size_t size)
{
    void *mem;

    size = vm_malloc_prepare(objspace, size);
    TRY_WITH_GC(mem = malloc(size));
    return vm_malloc_fixup(objspace, mem, size);
}

static void *
vm_xrealloc(rb_objspace_t *objspace, void *ptr, size_t size)
{
    void *mem;

    if ((ssize_t)size < 0) {
	negative_size_allocation_error("negative re-allocation size");
    }
    if (!ptr) return vm_xmalloc(objspace, size);
    if (size == 0) {
	vm_xfree(objspace, ptr);
	return 0;
    }
    if (ruby_gc_stress && !ruby_disable_gc_stress)
	garbage_collect_with_gvl(objspace);

#if CALC_EXACT_MALLOC_SIZE
    size += sizeof(size_t);
    objspace->malloc_params.allocated_size -= size;
    ptr = (size_t *)ptr - 1;
#endif

    mem = realloc(ptr, size);
    if (!mem) {
	if (garbage_collect_with_gvl(objspace)) {
	    mem = realloc(ptr, size);
	}
	if (!mem) {
	    ruby_memerror();
        }
    }
    malloc_increase += size;

#if CALC_EXACT_MALLOC_SIZE
    objspace->malloc_params.allocated_size += size;
    ((size_t *)mem)[0] = size;
    mem = (size_t *)mem + 1;
#endif

    return mem;
}

static void
vm_xfree(rb_objspace_t *objspace, void *ptr)
{
#if CALC_EXACT_MALLOC_SIZE
    size_t size;
    ptr = ((size_t *)ptr) - 1;
    size = ((size_t*)ptr)[0];
    if (size) {
	objspace->malloc_params.allocated_size -= size;
	objspace->malloc_params.allocations--;
    }
#endif

    free(ptr);
}

void *
ruby_xmalloc(size_t size)
{
    return vm_xmalloc(&rb_objspace, size);
}

static inline size_t
xmalloc2_size(size_t n, size_t size)
{
    size_t len = size * n;
    if (n != 0 && size != len / n) {
	rb_raise(rb_eArgError, "malloc: possible integer overflow");
    }
    return len;
}

void *
ruby_xmalloc2(size_t n, size_t size)
{
    return vm_xmalloc(&rb_objspace, xmalloc2_size(n, size));
}

static void *
vm_xcalloc(rb_objspace_t *objspace, size_t count, size_t elsize)
{
    void *mem;
    size_t size;

    size = xmalloc2_size(count, elsize);
    size = vm_malloc_prepare(objspace, size);

    TRY_WITH_GC(mem = calloc(1, size));
    return vm_malloc_fixup(objspace, mem, size);
}

void *
ruby_xcalloc(size_t n, size_t size)
{
    return vm_xcalloc(&rb_objspace, n, size);
}

void *
ruby_xrealloc(void *ptr, size_t size)
{
    return vm_xrealloc(&rb_objspace, ptr, size);
}

void *
ruby_xrealloc2(void *ptr, size_t n, size_t size)
{
    size_t len = size * n;
    if (n != 0 && size != len / n) {
	rb_raise(rb_eArgError, "realloc: possible integer overflow");
    }
    return ruby_xrealloc(ptr, len);
}

void
ruby_xfree(void *x)
{
    if (x)
	vm_xfree(&rb_objspace, x);
}

#ifdef POOL_ALLOC_API
/* POOL ALLOC API */
#define POOL_ALLOC_PART 2
#include "pool_alloc.inc.h"
#undef POOL_ALLOC_PART

void
ruby_xpool_free(void *ptr)
{
    pool_free_entry((void**)ptr);
}

#define CONCRET_POOL_MALLOC(pnts) \
void * ruby_xpool_malloc_##pnts##p () { \
    return pool_alloc_entry(&rb_objspace.pool_headers->p##pnts ); \
}
CONCRET_POOL_MALLOC(6)
CONCRET_POOL_MALLOC(11)
#undef CONCRET_POOL_MALLOC

#endif

/*
 *  call-seq:
 *     GC.enable    -> true or false
 *
 *  Enables garbage collection, returning <code>true</code> if garbage
 *  collection was previously disabled.
 *
 *     GC.disable   #=> false
 *     GC.enable    #=> true
 *     GC.enable    #=> false
 *
 */

VALUE
rb_gc_enable(void)
{
    rb_objspace_t *objspace = &rb_objspace;
    int old = dont_gc;

    dont_gc = FALSE;
    return old ? Qtrue : Qfalse;
}

/*
 *  call-seq:
 *     GC.disable    -> true or false
 *
 *  Disables garbage collection, returning <code>true</code> if garbage
 *  collection was already disabled.
 *
 *     GC.disable   #=> false
 *     GC.disable   #=> true
 *
 */

VALUE
rb_gc_disable(void)
{
    rb_objspace_t *objspace = &rb_objspace;
    int old = dont_gc;

    dont_gc = TRUE;
    return old ? Qtrue : Qfalse;
}

VALUE rb_mGC;

void
rb_gc_register_mark_object(VALUE obj)
{
    VALUE ary = GET_THREAD()->vm->mark_object_ary;
    rb_ary_push(ary, obj);
}

void
rb_gc_register_address(VALUE *addr)
{
    rb_objspace_t *objspace = &rb_objspace;
    struct gc_list *tmp;

    tmp = ALLOC(struct gc_list);
    tmp->next = global_List;
    tmp->varptr = addr;
    global_List = tmp;
}

void
rb_gc_unregister_address(VALUE *addr)
{
    rb_objspace_t *objspace = &rb_objspace;
    struct gc_list *tmp = global_List;

    if (tmp->varptr == addr) {
	global_List = tmp->next;
	xfree(tmp);
	return;
    }
    while (tmp->next) {
	if (tmp->next->varptr == addr) {
	    struct gc_list *t = tmp->next;

	    tmp->next = tmp->next->next;
	    xfree(t);
	    break;
	}
	tmp = tmp->next;
    }
}

static void
allocate_sorted_heaps(rb_objspace_t *objspace, size_t next_heaps_length)
{
    struct heaps_header **p;
    struct heaps_slot *slot;
    size_t size, add, i;

    size = next_heaps_length*sizeof(struct heaps_header *);
    add = next_heaps_length - heaps_used;

    if (heaps_used > 0) {
	p = (struct heaps_header **)realloc(objspace->heap.sorted, size);
	if (p) objspace->heap.sorted = p;
    }
    else {
	p = objspace->heap.sorted = (struct heaps_header **)malloc(size);
    }

    if (p == 0) {
	during_gc = 0;
	rb_memerror();
    }

    for (i = 0; i < add; i++) {
        slot = (struct heaps_slot *)malloc(HEAP_SLOT_SIZE);
        if (slot == 0) {
            during_gc = 0;
            rb_memerror();
            return;
        }
        slot->free_next = objspace->heap.reserve_slots;
        objspace->heap.reserve_slots = slot;
    }
}

static void *
aligned_malloc(size_t alignment, size_t size)
{
    void *res;

#if defined __MINGW32__
    res = __mingw_aligned_malloc(size, alignment);
#elif defined _WIN32 && !defined __CYGWIN__
    res = _aligned_malloc(size, alignment);
#elif defined(HAVE_POSIX_MEMALIGN)
    if (posix_memalign(&res, alignment, size) == 0) {
        return res;
    }
    else {
        return NULL;
    }
#elif defined(HAVE_MEMALIGN)
    res = memalign(alignment, size);
#else
    char* aligned;
    res = malloc(alignment + size + sizeof(void*));
    aligned = (char*)res + alignment + sizeof(void*);
    aligned -= ((VALUE)aligned & (alignment - 1));
    ((void**)aligned)[-1] = res;
    res = (void*)aligned;
#endif

#if defined(_DEBUG) || defined(GC_DEBUG)
    /* alignment must be a power of 2 */
    assert((alignment - 1) & alignment == 0);
    assert(alignment % sizeof(void*) == 0);
#endif
    return res;
}

static void
aligned_free(void *ptr)
{
#if defined __MINGW32__
    __mingw_aligned_free(ptr);
#elif defined _WIN32 && !defined __CYGWIN__
    _aligned_free(ptr);
#elif defined(HAVE_MEMALIGN) || defined(HAVE_POSIX_MEMALIGN)
    free(ptr);
#else
    free(((void**)ptr)[-1]);
#endif
}

static void
link_free_heap_slot(rb_objspace_t *objspace, struct heaps_slot *slot)
{
    slot->free_next = objspace->heap.free_slots;
    objspace->heap.free_slots = slot;
}

static void
unlink_free_heap_slot(rb_objspace_t *objspace, struct heaps_slot *slot)
{
    objspace->heap.free_slots = slot->free_next;
    slot->free_next = NULL;
}

static void
assign_heap_slot(rb_objspace_t *objspace)
{
    RVALUE *p, *pend;
    struct heaps_header *membase;
    struct heaps_slot *slot;
    size_t hi, lo, mid;
    size_t objs;

    objs = HEAP_OBJ_LIMIT;
    membase = (struct heaps_header*)aligned_malloc(HEAP_ALIGN, HEAP_SIZE);
    if (membase == 0) {
	during_gc = 0;
	rb_memerror();
    }
    assert(objspace->heap.reserve_slots != NULL);
    slot = objspace->heap.reserve_slots;
    objspace->heap.reserve_slots = slot->free_next;
    MEMZERO((void*)slot, struct heaps_slot, 1);

    slot->next = heaps;
    if (heaps) heaps->prev = slot;
    heaps = slot;

    p = (RVALUE*)((VALUE)membase + sizeof(struct heaps_header));
    if ((VALUE)p % sizeof(RVALUE) != 0) {
       p = (RVALUE*)((VALUE)p + sizeof(RVALUE) - ((VALUE)p % sizeof(RVALUE)));
       objs = (HEAP_SIZE - (size_t)((VALUE)p - (VALUE)membase))/sizeof(RVALUE);
    }

    lo = 0;
    hi = heaps_used;
    while (lo < hi) {
	register struct heaps_header *mid_membase;
	mid = (lo + hi) / 2;
        mid_membase = objspace->heap.sorted[mid];
	if (mid_membase < membase) {
	    lo = mid + 1;
	}
	else if (mid_membase > membase) {
	    hi = mid;
	}
	else {
	    rb_bug("same heap slot is allocated: %p at %"PRIuVALUE, (void *)membase, (VALUE)mid);
	}
    }
    if (hi < heaps_used) {
	MEMMOVE(&objspace->heap.sorted[hi+1], &objspace->heap.sorted[hi], struct heaps_header*, heaps_used - hi);
    }
    objspace->heap.sorted[hi] = membase;
    membase->start = p;
    membase->end = (p + objs);
    membase->base = heaps;
    membase->bits = heaps->bits;
    membase->limit = objs;
    membase->metadata = NULL;
    heaps->membase = membase;
    memset(heaps->bits, 0, HEAP_BITMAP_LIMIT * sizeof(uintptr_t));
    pend = p + objs;
    if (lomem == 0 || lomem > p) lomem = p;
    if (himem < pend) himem = pend;
    heaps_used++;

    while (p < pend) {
	p->as.free.flags = 0;
	p->as.free.next = heaps->freelist;
	heaps->freelist = p;
	p++;
    }
    link_free_heap_slot(objspace, heaps);
}

static void
add_heap_slots(rb_objspace_t *objspace, size_t add)
{
    size_t i;
    size_t next_heaps_length;

    next_heaps_length = heaps_used + add;

    if (next_heaps_length > heaps_length) {
        allocate_sorted_heaps(objspace, next_heaps_length);
        heaps_length = next_heaps_length;
    }

    for (i = 0; i < add; i++) {
        assign_heap_slot(objspace);
    }
    heaps_inc = 0;
}

static void
init_heap(rb_objspace_t *objspace)
{
    add_heap_slots(objspace, HEAP_MIN_SLOTS / HEAP_OBJ_LIMIT);
#ifdef USE_SIGALTSTACK
    {
	/* altstack of another threads are allocated in another place */
	rb_thread_t *th = GET_THREAD();
	void *tmp = th->altstack;
	th->altstack = malloc(ALT_STACK_SIZE);
	free(tmp); /* free previously allocated area */
    }
#endif

    objspace->profile.invoke_time = getrusage_time();
    finalizer_table = st_init_numtable();
}

static void
initial_expand_heap(rb_objspace_t *objspace)
{
    size_t min_size = initial_heap_min_slots / HEAP_OBJ_LIMIT;

    if (min_size > heaps_used) {
        add_heap_slots(objspace, min_size - heaps_used);
    }
}

static void
set_heaps_increment(rb_objspace_t *objspace)
{
    size_t next_heaps_length = (size_t)(heaps_used * initial_growth_factor);

    if (next_heaps_length == heaps_used) {
        next_heaps_length++;
    }

    heaps_inc = next_heaps_length - heaps_used;

    if (next_heaps_length > heaps_length) {
	allocate_sorted_heaps(objspace, next_heaps_length);
        heaps_length = next_heaps_length;
    }
}

static int
heaps_increment(rb_objspace_t *objspace)
{
    if (heaps_inc > 0) {
        assign_heap_slot(objspace);
	heaps_inc--;
	return TRUE;
    }
    return FALSE;
}

int
rb_during_gc(void)
{
    rb_objspace_t *objspace = &rb_objspace;
    return during_gc;
}

#define RANY(o) ((RVALUE*)(o))
#define has_free_object (objspace->heap.free_slots && objspace->heap.free_slots->freelist)

VALUE
rb_newobj(void)
{
    rb_objspace_t *objspace = &rb_objspace;
    VALUE obj;

    if (UNLIKELY(during_gc)) {
	dont_gc = 1;
	during_gc = 0;
	rb_bug("object allocation during garbage collection phase");
    }

    if (UNLIKELY(ruby_gc_stress && !ruby_disable_gc_stress)) {
	if (!garbage_collect(objspace, 0)) {
	    during_gc = 0;
	    rb_memerror();
	}
    }

    if (UNLIKELY(!has_free_object)) {
	if (!gc_lazy_sweep(objspace)) {
	    during_gc = 0;
	    rb_memerror();
	}
    }

    obj = (VALUE)objspace->heap.free_slots->freelist;
    objspace->heap.free_slots->freelist = RANY(obj)->as.free.next;
    if (objspace->heap.free_slots->freelist == NULL) {
        unlink_free_heap_slot(objspace, objspace->heap.free_slots);
    }

    MEMZERO((void*)obj, RVALUE, 1);
    objspace->total_allocated_object_num++;

    if (UNLIKELY(track_metadata)) {
        struct heaps_header *heap = GET_HEAP_HEADER(obj);
        if (!heap->metadata)
            heap->metadata = calloc(HEAP_OBJ_LIMIT+1, sizeof(rb_obj_metadata_t));
        if (heap->metadata) {
            rb_obj_metadata_t *meta = &heap->metadata[NUM_IN_SLOT(obj)];
            meta->file = rb_sourcefilename();
            meta->line = rb_sourceline();
        }
    }

    return obj;
}

NODE*
rb_node_newnode(enum node_type type, VALUE a0, VALUE a1, VALUE a2)
{
    NODE *n = (NODE*)rb_newobj();

    n->flags |= T_NODE;
    nd_set_type(n, type);

    n->u1.value = a0;
    n->u2.value = a1;
    n->u3.value = a2;

    return n;
}

VALUE
rb_data_object_alloc(VALUE klass, void *datap, RUBY_DATA_FUNC dmark, RUBY_DATA_FUNC dfree)
{
    NEWOBJ(data, struct RData);
    if (klass) Check_Type(klass, T_CLASS);
    OBJSETUP(data, klass, T_DATA);
    data->data = datap;
    data->dfree = dfree;
    data->dmark = dmark;

    return (VALUE)data;
}

VALUE
rb_data_typed_object_alloc(VALUE klass, void *datap, const rb_data_type_t *type)
{
    NEWOBJ(data, struct RTypedData);

    if (klass) Check_Type(klass, T_CLASS);

    OBJSETUP(data, klass, T_DATA);

    data->data = datap;
    data->typed_flag = 1;
    data->type = type;

    return (VALUE)data;
}

size_t
rb_objspace_data_type_memsize(VALUE obj)
{
    if (RTYPEDDATA_P(obj) && RTYPEDDATA_TYPE(obj)->function.dsize) {
	return RTYPEDDATA_TYPE(obj)->function.dsize(RTYPEDDATA_DATA(obj));
    }
    else {
	return 0;
    }
}

const char *
rb_objspace_data_type_name(VALUE obj)
{
    if (RTYPEDDATA_P(obj)) {
	return RTYPEDDATA_TYPE(obj)->wrap_struct_name;
    }
    else {
	return 0;
    }
}

#ifdef __ia64
#define SET_STACK_END (SET_MACHINE_STACK_END(&th->machine_stack_end), th->machine_register_stack_end = rb_ia64_bsp())
#else
#define SET_STACK_END SET_MACHINE_STACK_END(&th->machine_stack_end)
#endif

#define STACK_START (th->machine_stack_start)
#define STACK_END (th->machine_stack_end)
#define STACK_LEVEL_MAX (th->machine_stack_maxsize/sizeof(VALUE))

#if STACK_GROW_DIRECTION < 0
# define STACK_LENGTH  (size_t)(STACK_START - STACK_END)
#elif STACK_GROW_DIRECTION > 0
# define STACK_LENGTH  (size_t)(STACK_END - STACK_START + 1)
#else
# define STACK_LENGTH  ((STACK_END < STACK_START) ? (size_t)(STACK_START - STACK_END) \
			: (size_t)(STACK_END - STACK_START + 1))
#endif
#if !STACK_GROW_DIRECTION
int ruby_stack_grow_direction;
int
ruby_get_stack_grow_direction(volatile VALUE *addr)
{
    VALUE *end;
    SET_MACHINE_STACK_END(&end);

    if (end > addr) return ruby_stack_grow_direction = 1;
    return ruby_stack_grow_direction = -1;
}
#endif

#define GC_LEVEL_MAX 250
#define STACKFRAME_FOR_GC_MARK (GC_LEVEL_MAX * GC_MARK_STACKFRAME_WORD)

size_t
ruby_stack_length(VALUE **p)
{
    rb_thread_t *th = GET_THREAD();
    SET_STACK_END;
    if (p) *p = STACK_UPPER(STACK_END, STACK_START, STACK_END);
    return STACK_LENGTH;
}

static int
stack_check(int water_mark)
{
    int ret;
    rb_thread_t *th = GET_THREAD();
    SET_STACK_END;
    ret = STACK_LENGTH > STACK_LEVEL_MAX - water_mark;
#ifdef __ia64
    if (!ret) {
        ret = (VALUE*)rb_ia64_bsp() - th->machine_register_stack_start >
              th->machine_register_stack_maxsize/sizeof(VALUE) - water_mark;
    }
#endif
    return ret;
}

#define STACKFRAME_FOR_CALL_CFUNC 512

int
ruby_stack_check(void)
{
#if defined(POSIX_SIGNAL) && defined(SIGSEGV) && defined(HAVE_SIGALTSTACK)
    return 0;
#else
    return stack_check(STACKFRAME_FOR_CALL_CFUNC);
#endif
}

static void
init_mark_stack(rb_objspace_t *objspace)
{
    mark_stack_overflow = 0;
    mark_stack_ptr = mark_stack;
}

#define MARK_STACK_EMPTY (mark_stack_ptr == mark_stack)

static void gc_mark(rb_objspace_t *objspace, VALUE ptr, int lev);
static void gc_mark_children(rb_objspace_t *objspace, VALUE ptr, int lev);

static void
gc_mark_all(rb_objspace_t *objspace)
{
    RVALUE *p, *pend;
    size_t i;

    init_mark_stack(objspace);
    for (i = 0; i < heaps_used; i++) {
	p = objspace->heap.sorted[i]->start; pend = objspace->heap.sorted[i]->end;
	while (p < pend) {
	    if (MARKED_IN_BITMAP(GET_HEAP_BITMAP(p), p) &&
		p->as.basic.flags) {
		gc_mark_children(objspace, (VALUE)p, 0);
	    }
	    p++;
	}
    }
}

static void
gc_mark_rest(rb_objspace_t *objspace)
{
    VALUE tmp_arry[MARK_STACK_MAX];
    VALUE *p;

    p = (mark_stack_ptr - mark_stack) + tmp_arry;
    MEMCPY(tmp_arry, mark_stack, VALUE, p - tmp_arry);

    init_mark_stack(objspace);
    while (p != tmp_arry) {
	p--;
	gc_mark_children(objspace, *p, 0);
    }
}

static inline int
is_pointer_to_heap(rb_objspace_t *objspace, void *ptr)
{
    register RVALUE *p = RANY(ptr);
    register struct heaps_header *heap;
    register size_t hi, lo, mid;

    if (p < lomem || p > himem) return FALSE;
    if ((VALUE)p % sizeof(RVALUE) != 0) return FALSE;
    heap = GET_HEAP_HEADER(p);

    /* check if p looks like a pointer using bsearch*/
    lo = 0;
    hi = heaps_used;
    while (lo < hi) {
	mid = (lo + hi) / 2;
        if (heap > objspace->heap.sorted[mid]) {
            lo = mid + 1;
        }
        else if (heap < objspace->heap.sorted[mid]) {
            hi = mid;
        }
        else {
            return (p >= heap->start && p < heap->end) ? TRUE : FALSE;
        }
    }
    return FALSE;
}

static void
mark_locations_array(rb_objspace_t *objspace, register VALUE *x, register long n)
{
    VALUE v;
    while (n--) {
        v = *x;
        VALGRIND_MAKE_MEM_DEFINED(&v, sizeof(v));
	if (is_pointer_to_heap(objspace, (void *)v)) {
	    gc_mark(objspace, v, 0);
	}
	x++;
    }
}

static void
gc_mark_locations(rb_objspace_t *objspace, VALUE *start, VALUE *end)
{
    long n;

    if (end <= start) return;
    n = end - start;
    mark_locations_array(objspace, start, n);
}

void
rb_gc_mark_locations(VALUE *start, VALUE *end)
{
    gc_mark_locations(&rb_objspace, start, end);
}

#define rb_gc_mark_locations(start, end) gc_mark_locations(objspace, (start), (end))

struct mark_tbl_arg {
    rb_objspace_t *objspace;
    int lev;
};

static int
mark_entry(st_data_t key, st_data_t value, st_data_t data)
{
    struct mark_tbl_arg *arg = (void*)data;
    gc_mark(arg->objspace, (VALUE)value, arg->lev);
    return ST_CONTINUE;
}

static void
mark_tbl(rb_objspace_t *objspace, st_table *tbl, int lev)
{
    struct mark_tbl_arg arg;
    if (!tbl || tbl->num_entries == 0) return;
    arg.objspace = objspace;
    arg.lev = lev;
    st_foreach(tbl, mark_entry, (st_data_t)&arg);
}

static int
mark_key(st_data_t key, st_data_t value, st_data_t data)
{
    struct mark_tbl_arg *arg = (void*)data;
    gc_mark(arg->objspace, (VALUE)key, arg->lev);
    return ST_CONTINUE;
}

static void
mark_set(rb_objspace_t *objspace, st_table *tbl, int lev)
{
    struct mark_tbl_arg arg;
    if (!tbl) return;
    arg.objspace = objspace;
    arg.lev = lev;
    st_foreach(tbl, mark_key, (st_data_t)&arg);
}

void
rb_mark_set(st_table *tbl)
{
    mark_set(&rb_objspace, tbl, 0);
}

static int
mark_keyvalue(st_data_t key, st_data_t value, st_data_t data)
{
    struct mark_tbl_arg *arg = (void*)data;
    gc_mark(arg->objspace, (VALUE)key, arg->lev);
    gc_mark(arg->objspace, (VALUE)value, arg->lev);
    return ST_CONTINUE;
}

static void
mark_hash(rb_objspace_t *objspace, st_table *tbl, int lev)
{
    struct mark_tbl_arg arg;
    if (!tbl) return;
    arg.objspace = objspace;
    arg.lev = lev;
    st_foreach(tbl, mark_keyvalue, (st_data_t)&arg);
}

void
rb_mark_hash(st_table *tbl)
{
    mark_hash(&rb_objspace, tbl, 0);
}

static void
mark_method_entry(rb_objspace_t *objspace, const rb_method_entry_t *me, int lev)
{
    const rb_method_definition_t *def = me->def;

    gc_mark(objspace, me->klass, lev);
    if (!def) return;
    switch (def->type) {
      case VM_METHOD_TYPE_ISEQ:
	gc_mark(objspace, def->body.iseq->self, lev);
	break;
      case VM_METHOD_TYPE_BMETHOD:
	gc_mark(objspace, def->body.proc, lev);
	break;
      case VM_METHOD_TYPE_ATTRSET:
      case VM_METHOD_TYPE_IVAR:
	gc_mark(objspace, def->body.attr.location, lev);
	break;
      default:
	break; /* ignore */
    }
}

void
rb_mark_method_entry(const rb_method_entry_t *me)
{
    mark_method_entry(&rb_objspace, me, 0);
}

static int
mark_method_entry_i(ID key, const rb_method_entry_t *me, st_data_t data)
{
    struct mark_tbl_arg *arg = (void*)data;
    mark_method_entry(arg->objspace, me, arg->lev);
    return ST_CONTINUE;
}

static void
mark_m_tbl(rb_objspace_t *objspace, st_table *tbl, int lev)
{
    struct mark_tbl_arg arg;
    if (!tbl) return;
    arg.objspace = objspace;
    arg.lev = lev;
    st_foreach(tbl, mark_method_entry_i, (st_data_t)&arg);
}

static int
free_method_entry_i(ID key, rb_method_entry_t *me, st_data_t data)
{
    if (!me->mark) {
	rb_free_method_entry(me);
    }
    return ST_CONTINUE;
}

void
rb_free_m_table(st_table *tbl)
{
    st_foreach(tbl, free_method_entry_i, 0);
    st_free_table(tbl);
}

static int
free_method_cache_entry_i(ID key, method_cache_entry_t *entry, st_data_t data)
{
  free(entry);
  return SA_CONTINUE;
}

void
rb_free_mc_table(sa_table *tbl)
{
  sa_foreach(tbl, free_method_cache_entry_i, 0);
  sa_free_table(tbl);
}

static int
mark_const_entry_i(ID key, const rb_const_entry_t *ce, st_data_t data)
{
    struct mark_tbl_arg *arg = (void*)data;
    gc_mark(arg->objspace, ce->value, arg->lev);
    return ST_CONTINUE;
}

static void
mark_const_tbl(rb_objspace_t *objspace, st_table *tbl, int lev)
{
    struct mark_tbl_arg arg;
    if (!tbl) return;
    arg.objspace = objspace;
    arg.lev = lev;
    st_foreach(tbl, mark_const_entry_i, (st_data_t)&arg);
}

static int
free_const_entry_i(ID key, rb_const_entry_t *ce, st_data_t data)
{
    xfree(ce);
    return ST_CONTINUE;
}

void
rb_free_const_table(st_table *tbl)
{
    st_foreach(tbl, free_const_entry_i, 0);
    st_free_table(tbl);
}

void
rb_mark_tbl(st_table *tbl)
{
    mark_tbl(&rb_objspace, tbl, 0);
}

void
rb_gc_mark_maybe(VALUE obj)
{
    if (is_pointer_to_heap(&rb_objspace, (void *)obj)) {
	gc_mark(&rb_objspace, obj, 0);
    }
}

static int
gc_mark_ptr(rb_objspace_t *objspace, VALUE ptr)
{
    register uintptr_t *bits = GET_HEAP_BITMAP(ptr);
    if (MARKED_IN_BITMAP(bits, ptr)) return 0;
    MARK_IN_BITMAP(bits, ptr);
    objspace->heap.marked_num++;
    return 1;
}

static void
gc_mark(rb_objspace_t *objspace, VALUE ptr, int lev)
{
    register RVALUE *obj;

    obj = RANY(ptr);
    if (rb_special_const_p(ptr)) return; /* special const not marked */
    if (obj->as.basic.flags == 0) return;       /* free cell */
    if (!gc_mark_ptr(objspace, ptr)) return;	/* already marked */

    if (lev > GC_LEVEL_MAX || (lev == 0 && stack_check(STACKFRAME_FOR_GC_MARK))) {
	if (!mark_stack_overflow) {
	    if (mark_stack_ptr - mark_stack < MARK_STACK_MAX) {
		*mark_stack_ptr = ptr;
		mark_stack_ptr++;
	    }
	    else {
		mark_stack_overflow = 1;
	    }
	}
	return;
    }
    gc_mark_children(objspace, ptr, lev+1);
}

void
rb_gc_mark(VALUE ptr)
{
    gc_mark(&rb_objspace, ptr, 0);
}

static inline rb_obj_metadata_t *rb_obj_get_metadata(VALUE obj);

static void
gc_mark_children(rb_objspace_t *objspace, VALUE ptr, int lev)
{
    register RVALUE *obj = RANY(ptr);
    register uintptr_t *bits;
    register rb_obj_metadata_t *meta;

    goto marking;		/* skip */

  again:
    obj = RANY(ptr);
    if (rb_special_const_p(ptr)) return; /* special const not marked */
    if (obj->as.basic.flags == 0) return;       /* free cell */
    bits = GET_HEAP_BITMAP(ptr);
    if (MARKED_IN_BITMAP(bits, ptr)) return;  /* already marked */
    MARK_IN_BITMAP(bits, ptr);
    objspace->heap.marked_num++;

  marking:
    if (FL_TEST(obj, FL_EXIVAR)) {
	rb_mark_generic_ivar(ptr);
    }

    if ((meta = rb_obj_get_metadata(ptr)) && RTEST(meta->file))
        gc_mark(objspace, meta->file, lev);

    switch (BUILTIN_TYPE(obj)) {
      case T_NIL:
      case T_FIXNUM:
	rb_bug("rb_gc_mark() called for broken object");
	break;

      case T_NODE:
	switch (nd_type(obj)) {
	  case NODE_IF:		/* 1,2,3 */
	  case NODE_FOR:
	  case NODE_ITER:
	  case NODE_WHEN:
	  case NODE_MASGN:
	  case NODE_RESCUE:
	  case NODE_RESBODY:
	  case NODE_CLASS:
	  case NODE_BLOCK_PASS:
	    gc_mark(objspace, (VALUE)obj->as.node.u2.node, lev);
	    /* fall through */
	  case NODE_BLOCK:	/* 1,3 */
	  case NODE_OPTBLOCK:
	  case NODE_ARRAY:
	  case NODE_DSTR:
	  case NODE_DXSTR:
	  case NODE_DREGX:
	  case NODE_DREGX_ONCE:
	  case NODE_ENSURE:
	  case NODE_CALL:
	  case NODE_DEFS:
	  case NODE_OP_ASGN1:
	  case NODE_ARGS:
	    gc_mark(objspace, (VALUE)obj->as.node.u1.node, lev);
	    /* fall through */
	  case NODE_SUPER:	/* 3 */
	  case NODE_FCALL:
	  case NODE_DEFN:
	  case NODE_ARGS_AUX:
	    ptr = (VALUE)obj->as.node.u3.node;
	    goto again;

	  case NODE_WHILE:	/* 1,2 */
	  case NODE_UNTIL:
	  case NODE_AND:
	  case NODE_OR:
	  case NODE_CASE:
	  case NODE_SCLASS:
	  case NODE_DOT2:
	  case NODE_DOT3:
	  case NODE_FLIP2:
	  case NODE_FLIP3:
	  case NODE_MATCH2:
	  case NODE_MATCH3:
	  case NODE_OP_ASGN_OR:
	  case NODE_OP_ASGN_AND:
	  case NODE_MODULE:
	  case NODE_ALIAS:
	  case NODE_VALIAS:
	  case NODE_ARGSCAT:
	    gc_mark(objspace, (VALUE)obj->as.node.u1.node, lev);
	    /* fall through */
	  case NODE_GASGN:	/* 2 */
	  case NODE_LASGN:
	  case NODE_DASGN:
	  case NODE_DASGN_CURR:
	  case NODE_IASGN:
	  case NODE_IASGN2:
	  case NODE_CVASGN:
	  case NODE_COLON3:
	  case NODE_OPT_N:
	  case NODE_EVSTR:
	  case NODE_UNDEF:
	  case NODE_POSTEXE:
	    ptr = (VALUE)obj->as.node.u2.node;
	    goto again;

	  case NODE_HASH:	/* 1 */
	  case NODE_LIT:
	  case NODE_STR:
	  case NODE_XSTR:
	  case NODE_DEFINED:
	  case NODE_MATCH:
	  case NODE_RETURN:
	  case NODE_BREAK:
	  case NODE_NEXT:
	  case NODE_YIELD:
	  case NODE_COLON2:
	  case NODE_SPLAT:
	  case NODE_TO_ARY:
	    ptr = (VALUE)obj->as.node.u1.node;
	    goto again;

	  case NODE_SCOPE:	/* 2,3 */
	  case NODE_CDECL:
	  case NODE_OPT_ARG:
	    gc_mark(objspace, (VALUE)obj->as.node.u3.node, lev);
	    ptr = (VALUE)obj->as.node.u2.node;
	    goto again;

	  case NODE_ZARRAY:	/* - */
	  case NODE_ZSUPER:
	  case NODE_VCALL:
	  case NODE_GVAR:
	  case NODE_LVAR:
	  case NODE_DVAR:
	  case NODE_IVAR:
	  case NODE_CVAR:
	  case NODE_NTH_REF:
	  case NODE_BACK_REF:
	  case NODE_REDO:
	  case NODE_RETRY:
	  case NODE_SELF:
	  case NODE_NIL:
	  case NODE_TRUE:
	  case NODE_FALSE:
	  case NODE_ERRINFO:
	  case NODE_BLOCK_ARG:
	    break;
	  case NODE_ALLOCA:
	    mark_locations_array(objspace,
				 (VALUE*)obj->as.node.u1.value,
				 obj->as.node.u3.cnt);
	    ptr = (VALUE)obj->as.node.u2.node;
	    goto again;

	  default:		/* unlisted NODE */
	    if (is_pointer_to_heap(objspace, obj->as.node.u1.node)) {
		gc_mark(objspace, (VALUE)obj->as.node.u1.node, lev);
	    }
	    if (is_pointer_to_heap(objspace, obj->as.node.u2.node)) {
		gc_mark(objspace, (VALUE)obj->as.node.u2.node, lev);
	    }
	    if (is_pointer_to_heap(objspace, obj->as.node.u3.node)) {
		gc_mark(objspace, (VALUE)obj->as.node.u3.node, lev);
	    }
	}
	return;			/* no need to mark class. */
    }

    gc_mark(objspace, obj->as.basic.klass, lev);
    switch (BUILTIN_TYPE(obj)) {
      case T_ICLASS:
      case T_CLASS:
      case T_MODULE:
	mark_m_tbl(objspace, RCLASS_M_TBL(obj), lev);
	if (!RCLASS_EXT(obj)) break;
	mark_tbl(objspace, RCLASS_IV_TBL(obj), lev);
	mark_const_tbl(objspace, RCLASS_CONST_TBL(obj), lev);
	ptr = RCLASS_SUPER(obj);
	goto again;

      case T_ARRAY:
	if (FL_TEST(obj, ELTS_SHARED)) {
	    ptr = obj->as.array.as.heap.aux.shared;
	    goto again;
	}
	else {
	    long i, len = RARRAY_LEN(obj);
	    VALUE *ptr = RARRAY_PTR(obj);
	    for (i=0; i < len; i++) {
		gc_mark(objspace, *ptr++, lev);
	    }
	}
	break;

      case T_HASH:
	mark_hash(objspace, obj->as.hash.ntbl, lev);
	ptr = obj->as.hash.ifnone;
	goto again;

      case T_STRING:
#define STR_ASSOC FL_USER3   /* copied from string.c */
	if (FL_TEST(obj, RSTRING_NOEMBED) && FL_ANY(obj, ELTS_SHARED|STR_ASSOC)) {
	    ptr = obj->as.string.as.heap.aux.shared;
	    goto again;
	}
	break;

      case T_DATA:
	if (RTYPEDDATA_P(obj)) {
	    RUBY_DATA_FUNC mark_func = obj->as.typeddata.type->function.dmark;
	    if (mark_func) (*mark_func)(DATA_PTR(obj));
	}
	else {
	    if (obj->as.data.dmark) (*obj->as.data.dmark)(DATA_PTR(obj));
	}
	break;

      case T_OBJECT:
        {
            long i, len = ROBJECT_NUMIV(obj);
	    VALUE *ptr = ROBJECT_IVPTR(obj);
            for (i  = 0; i < len; i++) {
		gc_mark(objspace, *ptr++, lev);
            }
        }
	break;

      case T_FILE:
        if (obj->as.file.fptr) {
            gc_mark(objspace, obj->as.file.fptr->pathv, lev);
            gc_mark(objspace, obj->as.file.fptr->tied_io_for_writing, lev);
            gc_mark(objspace, obj->as.file.fptr->writeconv_asciicompat, lev);
            gc_mark(objspace, obj->as.file.fptr->writeconv_pre_ecopts, lev);
            gc_mark(objspace, obj->as.file.fptr->encs.ecopts, lev);
            gc_mark(objspace, obj->as.file.fptr->write_lock, lev);
        }
        break;

      case T_REGEXP:
        gc_mark(objspace, obj->as.regexp.src, lev);
        break;

      case T_FLOAT:
      case T_BIGNUM:
      case T_ZOMBIE:
	break;

      case T_MATCH:
	gc_mark(objspace, obj->as.match.regexp, lev);
	if (obj->as.match.str) {
	    ptr = obj->as.match.str;
	    goto again;
	}
	break;

      case T_RATIONAL:
	gc_mark(objspace, obj->as.rational.num, lev);
	gc_mark(objspace, obj->as.rational.den, lev);
	break;

      case T_COMPLEX:
	gc_mark(objspace, obj->as.complex.real, lev);
	gc_mark(objspace, obj->as.complex.imag, lev);
	break;

      case T_STRUCT:
	{
	    long len = RSTRUCT_LEN(obj);
	    VALUE *ptr = RSTRUCT_PTR(obj);

	    while (len--) {
		gc_mark(objspace, *ptr++, lev);
	    }
	}
	break;

      default:
	rb_bug("rb_gc_mark(): unknown data type 0x%x(%p) %s",
	       BUILTIN_TYPE(obj), (void *)obj,
	       is_pointer_to_heap(objspace, obj) ? "corrupted object" : "non object");
    }
}

static int obj_free(rb_objspace_t *, VALUE);

static inline struct heaps_slot *
add_slot_local_freelist(rb_objspace_t *objspace, RVALUE *p)
{
    struct heaps_slot *slot;

    VALGRIND_MAKE_MEM_UNDEFINED((void*)p, sizeof(RVALUE));
    p->as.free.flags = 0;
    slot = GET_HEAP_SLOT(p);
    p->as.free.next = slot->freelist;
    slot->freelist = p;

    return slot;
}

static void free_unused_heap(rb_objspace_t *objspace, struct heaps_header *heap);

static void
finalize_list(rb_objspace_t *objspace, RVALUE *p)
{
    while (p) {
	RVALUE *tmp = p->as.free.next;
	run_final(objspace, (VALUE)p);
	if (!FL_TEST(p, FL_SINGLETON)) { /* not freeing page */
            objspace->total_freed_object_num++;
            objspace->heap.swept_num++;
            add_slot_local_freelist(objspace, p);
	}
	else {
            struct heaps_header *heap = GET_HEAP_HEADER(p);
            if (--heap->limit == 0) {
                free_unused_heap(objspace, heap);
            }
	}
	p = tmp;
    }
}

static void
unlink_heap_slot(rb_objspace_t *objspace, struct heaps_slot *slot)
{
    if (slot->prev)
        slot->prev->next = slot->next;
    if (slot->next)
        slot->next->prev = slot->prev;
    if (heaps == slot)
        heaps = slot->next;
    if (objspace->heap.sweep_slots == slot)
        objspace->heap.sweep_slots = slot->next;
    slot->prev = NULL;
    slot->next = NULL;
}

static void
free_unused_heap(rb_objspace_t *objspace, struct heaps_header *heap)
{
    register size_t hi, lo, mid;
    lo = 0;
    hi = heaps_used;
    while (lo < hi) {
        mid = (lo + hi) / 2;
        if (heap > objspace->heap.sorted[mid]) {
            lo = mid + 1;
        }
        else if (heap < objspace->heap.sorted[mid]) {
            hi = mid;
        }
        else {
            /* remove unused heap */
            struct heaps_slot* h = objspace->heap.sorted[mid]->base;
            h->free_next = objspace->heap.reserve_slots;
            objspace->heap.reserve_slots = h;
            if (objspace->heap.sorted[mid]->metadata)
                free(objspace->heap.sorted[mid]->metadata);
            aligned_free(objspace->heap.sorted[mid]);
            heaps_used--;
            MEMMOVE(objspace->heap.sorted + mid, objspace->heap.sorted + mid + 1,
                    struct heaps_header *, heaps_used - mid);
            return;
        }
    }
}

static void
gc_clear_slot_bits(struct heaps_slot *slot)
{
    memset(slot->bits, 0, HEAP_BITMAP_LIMIT * sizeof(uintptr_t));
}

static void
slot_sweep(rb_objspace_t *objspace, struct heaps_slot *sweep_slot)
{
    size_t freed_num = 0, empty_num = 0, final_num = 0;
    RVALUE *p, *pend;
    RVALUE *final = deferred_final_list;
    int deferred;
    uintptr_t *bits;

    p = sweep_slot->membase->start; pend = sweep_slot->membase->end;
    bits = sweep_slot->bits;
    while (p < pend) {
        if ((!(MARKED_IN_BITMAP(bits, p))) && BUILTIN_TYPE(p) != T_ZOMBIE) {
            if (p->as.basic.flags) {
                if ((deferred = obj_free(objspace, (VALUE)p)) ||
                    (FL_TEST(p, FL_FINALIZE))) {
                    if (!deferred) {
                        p->as.free.flags = T_ZOMBIE;
                        RDATA(p)->dfree = 0;
                    }
                    p->as.free.next = deferred_final_list;
                    deferred_final_list = p;
                    assert(BUILTIN_TYPE(p) == T_ZOMBIE);
                    final_num++;
                }
                else {
                    VALGRIND_MAKE_MEM_UNDEFINED((void*)p, sizeof(RVALUE));
                    p->as.free.flags = 0;
                    p->as.free.next = sweep_slot->freelist;
                    sweep_slot->freelist = p;
                    freed_num++;
                }
            }
            else {
                empty_num++;
            }
        }
        p++;
    }
    gc_clear_slot_bits(sweep_slot);
    if (final_num + freed_num + empty_num == sweep_slot->membase->limit &&
        heaps_dec > 0) {
        RVALUE *pp;
        heaps_dec--;

        for (pp = deferred_final_list; pp != final; pp = pp->as.free.next) {
	    RDATA(pp)->dmark = 0;
            pp->as.free.flags |= FL_SINGLETON; /* freeing page mark */
        }
        sweep_slot->membase->limit = final_num;
        unlink_heap_slot(objspace, sweep_slot);
        if (final_num == 0) {
            free_unused_heap(objspace, sweep_slot->membase);
        }
    }
    else {
        if (freed_num + empty_num > 0) {
            link_free_heap_slot(objspace, sweep_slot);
        }
        else {
            sweep_slot->free_next = NULL;
        }
        objspace->heap.swept_num += freed_num + empty_num;
    }
    objspace->total_freed_object_num += freed_num;
    objspace->heap.final_num += final_num;

    if (deferred_final_list && !finalizing) {
        rb_thread_t *th = GET_THREAD();
        if (th) {
            RUBY_VM_SET_FINALIZER_INTERRUPT(th);
        }
    }
}

static int
ready_to_gc(rb_objspace_t *objspace)
{
    if (dont_gc || during_gc) {
	if (!has_free_object) {
            if (!heaps_increment(objspace)) {
                set_heaps_increment(objspace);
                heaps_increment(objspace);
            }
	}
	return FALSE;
    }
    return TRUE;
}

static inline size_t
objspace_live_num(rb_objspace_t *objspace)
{
    return objspace->total_allocated_object_num - objspace->total_freed_object_num;
}

static void
before_gc_sweep(rb_objspace_t *objspace)
{
    size_t total_slots, marked_slots, unmarked_slots;

    total_slots = heaps_used * HEAP_OBJ_LIMIT;
    marked_slots = objspace->heap.marked_num;
    unmarked_slots = total_slots - marked_slots;

    objspace->heap.free_min = (size_t)(total_slots * 0.2);
    objspace->heap.free_max = (size_t)(total_slots * 0.5);

    if (objspace->heap.free_min < initial_free_min)
        objspace->heap.free_min = initial_free_min;
    if (objspace->heap.free_max < initial_free_min)
        objspace->heap.free_max = initial_free_min;

    if (unmarked_slots < objspace->heap.free_min) {
        heaps_dec = 0;
        set_heaps_increment(objspace);
        heaps_increment(objspace);
    } else if (unmarked_slots > objspace->heap.free_max) {
        heaps_inc = 0;
        heaps_dec = (size_t)((unmarked_slots - objspace->heap.free_max) / HEAP_OBJ_LIMIT);
    }

    objspace->heap.sweep_slots = heaps;
    objspace->heap.swept_num = 0;
    objspace->heap.free_slots = NULL;

    /* sweep unlinked method entries */
    if (GET_VM()->unlinked_method_entry_list) {
	rb_sweep_method_entry(GET_VM());
    }
}

static void
after_gc_sweep(rb_objspace_t *objspace)
{
    GC_PROF_SET_MALLOC_INFO;

    if (malloc_increase > malloc_limit) {
	malloc_limit += (size_t)((malloc_increase - malloc_limit) * (double)objspace->heap.marked_num / (heaps_used * HEAP_OBJ_LIMIT));
	if (malloc_limit < initial_malloc_limit) malloc_limit = initial_malloc_limit;
    }
    malloc_increase = 0;
}

static int
lazy_sweep(rb_objspace_t *objspace)
{
    struct heaps_slot *next;

    heaps_increment(objspace);
    while (objspace->heap.sweep_slots) {
        next = objspace->heap.sweep_slots->next;
	slot_sweep(objspace, objspace->heap.sweep_slots);
        objspace->heap.sweep_slots = next;
        if (has_free_object) {
            during_gc = 0;
            return TRUE;
        }
    }
    return FALSE;
}

static void
rest_sweep(rb_objspace_t *objspace)
{
    if (objspace->heap.sweep_slots) {
	while (objspace->heap.sweep_slots) {
	    lazy_sweep(objspace);
	}
	after_gc_sweep(objspace);
    }
}

static void gc_marks(rb_objspace_t *objspace);

static int
gc_lazy_sweep(rb_objspace_t *objspace)
{
    int res;
    INIT_GC_PROF_PARAMS;

    if (objspace->flags.dont_lazy_sweep)
        return garbage_collect(objspace, 0);


    if (!ready_to_gc(objspace)) return TRUE;

    during_gc++;
    GC_PROF_TIMER_START;
    GC_PROF_SWEEP_TIMER_START;

    if (objspace->heap.sweep_slots) {
        res = lazy_sweep(objspace);
        if (res) {
            GC_PROF_SWEEP_TIMER_STOP;
            GC_PROF_SET_MALLOC_INFO;
            GC_PROF_TIMER_STOP(Qfalse);
            return res;
        }
        after_gc_sweep(objspace);
    }
    else {
        if (heaps_increment(objspace)) {
            during_gc = 0;
            return TRUE;
        }
    }

    gc_marks(objspace);

    before_gc_sweep(objspace);

    GC_PROF_SWEEP_TIMER_START;
    if (!(res = lazy_sweep(objspace))) {
        after_gc_sweep(objspace);
        if (has_free_object) {
            res = TRUE;
            during_gc = 0;
        }
    }
    GC_PROF_SWEEP_TIMER_STOP;

    GC_PROF_TIMER_STOP(Qtrue);
    return res;
}

static void
gc_sweep(rb_objspace_t *objspace)
{
    struct heaps_slot *next;

    before_gc_sweep(objspace);

    while (objspace->heap.sweep_slots) {
        next = objspace->heap.sweep_slots->next;
	slot_sweep(objspace, objspace->heap.sweep_slots);
        objspace->heap.sweep_slots = next;
    }

    after_gc_sweep(objspace);

    during_gc = 0;
}

void
rb_gc_force_recycle(VALUE p)
{
    rb_objspace_t *objspace = &rb_objspace;
    struct heaps_slot *slot;

    objspace->total_freed_object_num++;
    if (MARKED_IN_BITMAP(GET_HEAP_BITMAP(p), p)) {
        add_slot_local_freelist(objspace, (RVALUE *)p);
    }
    else {
        objspace->heap.swept_num++;
        slot = add_slot_local_freelist(objspace, (RVALUE *)p);
        if (slot->free_next == NULL) {
            link_free_heap_slot(objspace, slot);
        }
    }
}

static inline void
make_deferred(RVALUE *p)
{
    p->as.basic.flags = (p->as.basic.flags & ~T_MASK) | T_ZOMBIE;
}

static inline void
make_io_deferred(RVALUE *p)
{
    rb_io_t *fptr = p->as.file.fptr;
    make_deferred(p);
    p->as.data.dfree = (void (*)(void*))rb_io_fptr_finalize;
    p->as.data.data = fptr;
}

static int
obj_free(rb_objspace_t *objspace, VALUE obj)
{
    switch (BUILTIN_TYPE(obj)) {
      case T_NIL:
      case T_FIXNUM:
      case T_TRUE:
      case T_FALSE:
	rb_bug("obj_free() called for broken object");
	break;
    }

    if (FL_TEST(obj, FL_EXIVAR)) {
	rb_free_generic_ivar((VALUE)obj);
	FL_UNSET(obj, FL_EXIVAR);
    }

    switch (BUILTIN_TYPE(obj)) {
      case T_OBJECT:
	if (!(RANY(obj)->as.basic.flags & ROBJECT_EMBED) &&
            RANY(obj)->as.object.as.heap.ivptr) {
	    xfree(RANY(obj)->as.object.as.heap.ivptr);
	}
	break;
      case T_MODULE:
      case T_CLASS:
	rb_free_m_table(RCLASS_M_TBL(obj));
	if (RCLASS_IV_TBL(obj)) {
	    st_free_table(RCLASS_IV_TBL(obj));
	}
	if (RCLASS_CONST_TBL(obj)) {
	    rb_free_const_table(RCLASS_CONST_TBL(obj));
	}
	if (RCLASS_IV_INDEX_TBL(obj)) {
	    st_free_table(RCLASS_IV_INDEX_TBL(obj));
	}
	if (RCLASS_SUBCLASSES(obj)) {
	  if (BUILTIN_TYPE(obj) == T_MODULE) {
	    rb_class_detatch_module_subclasses(obj);
	  } else {
	    rb_class_detatch_subclasses(obj);
	  }
	  RCLASS_SUBCLASSES(obj) = NULL;
	}
	if (RCLASS_MC_TBL(obj)) {
	  rb_free_mc_table(RCLASS_MC_TBL(obj));
	  RCLASS_MC_TBL(obj) = NULL;
	}
	rb_class_remove_from_module_subclasses(obj);
        rb_class_remove_from_super_subclasses(obj);
        xfree(RANY(obj)->as.klass.ptr);
        RANY(obj)->as.klass.ptr = NULL;
	break;
      case T_STRING:
	rb_str_free(obj);
	break;
      case T_ARRAY:
	rb_ary_free(obj);
	break;
      case T_HASH:
	if (RANY(obj)->as.hash.ntbl) {
	    st_free_table(RANY(obj)->as.hash.ntbl);
	}
	break;
      case T_REGEXP:
	if (RANY(obj)->as.regexp.ptr) {
	    onig_free(RANY(obj)->as.regexp.ptr);
	}
	break;
      case T_DATA:
	if (DATA_PTR(obj)) {
	    if (RTYPEDDATA_P(obj)) {
		RDATA(obj)->dfree = RANY(obj)->as.typeddata.type->function.dfree;
	    }
	    if (RANY(obj)->as.data.dfree == (RUBY_DATA_FUNC)-1) {
		xfree(DATA_PTR(obj));
	    }
	    else if (RANY(obj)->as.data.dfree) {
		make_deferred(RANY(obj));
		return 1;
	    }
	}
	break;
      case T_MATCH:
	if (RANY(obj)->as.match.rmatch) {
            struct rmatch *rm = RANY(obj)->as.match.rmatch;
	    onig_region_free(&rm->regs, 0);
            if (rm->char_offset)
		xfree(rm->char_offset);
	    xfree(rm);
	}
	break;
      case T_FILE:
	if (RANY(obj)->as.file.fptr) {
	    make_io_deferred(RANY(obj));
	    return 1;
	}
	break;
      case T_RATIONAL:
      case T_COMPLEX:
	break;
      case T_ICLASS:
	/* iClass shares table with the module */
	if (RCLASS_SUBCLASSES(obj)) {
	  rb_class_detatch_subclasses(obj);
	  RCLASS_SUBCLASSES(obj) = NULL;
	}
	rb_class_remove_from_module_subclasses(obj);
        rb_class_remove_from_super_subclasses(obj);
	xfree(RANY(obj)->as.klass.ptr);
        RANY(obj)->as.klass.ptr = NULL;
	break;

      case T_FLOAT:
	break;

      case T_BIGNUM:
	if (!(RBASIC(obj)->flags & RBIGNUM_EMBED_FLAG) && RBIGNUM_DIGITS(obj)) {
	    xfree(RBIGNUM_DIGITS(obj));
	}
	break;
      case T_NODE:
	switch (nd_type(obj)) {
	  case NODE_SCOPE:
	    if (RANY(obj)->as.node.u1.tbl) {
		xfree(RANY(obj)->as.node.u1.tbl);
	    }
	    break;
	  case NODE_ALLOCA:
	    xfree(RANY(obj)->as.node.u1.node);
	    break;
	}
	break;			/* no need to free iv_tbl */

      case T_STRUCT:
	if ((RBASIC(obj)->flags & RSTRUCT_EMBED_LEN_MASK) == 0 &&
	    RANY(obj)->as.rstruct.as.heap.ptr) {
	    xfree(RANY(obj)->as.rstruct.as.heap.ptr);
	}
	break;

      default:
	rb_bug("gc_sweep(): unknown data type 0x%x(%p)",
	       BUILTIN_TYPE(obj), (void*)obj);
    }

    return 0;
}

#define GC_NOTIFY 0

#if STACK_GROW_DIRECTION < 0
#define GET_STACK_BOUNDS(start, end, appendix) ((start) = STACK_END, (end) = STACK_START)
#elif STACK_GROW_DIRECTION > 0
#define GET_STACK_BOUNDS(start, end, appendix) ((start) = STACK_START, (end) = STACK_END+(appendix))
#else
#define GET_STACK_BOUNDS(start, end, appendix) \
    ((STACK_END < STACK_START) ? \
     ((start) = STACK_END, (end) = STACK_START) : ((start) = STACK_START, (end) = STACK_END+(appendix)))
#endif

#define numberof(array) (int)(sizeof(array) / sizeof((array)[0]))

static void
mark_current_machine_context(rb_objspace_t *objspace, rb_thread_t *th)
{
    union {
	rb_jmp_buf j;
	VALUE v[sizeof(rb_jmp_buf) / sizeof(VALUE)];
    } save_regs_gc_mark;
    VALUE *stack_start, *stack_end;

    FLUSH_REGISTER_WINDOWS;
    /* This assumes that all registers are saved into the jmp_buf (and stack) */
    rb_setjmp(save_regs_gc_mark.j);

    SET_STACK_END;
    GET_STACK_BOUNDS(stack_start, stack_end, 1);

    mark_locations_array(objspace, save_regs_gc_mark.v, numberof(save_regs_gc_mark.v));

    rb_gc_mark_locations(stack_start, stack_end);
#ifdef __ia64
    rb_gc_mark_locations(th->machine_register_stack_start, th->machine_register_stack_end);
#endif
#if defined(__mc68000__)
    mark_locations_array(objspace, (VALUE*)((char*)STACK_END + 2),
			 (STACK_START - STACK_END));
#endif
}

static void
gc_marks(rb_objspace_t *objspace)
{
    struct gc_list *list;
    rb_thread_t *th = GET_THREAD();
    GC_PROF_MARK_TIMER_START;

    objspace->heap.marked_num = 0;
    objspace->count++;


    SET_STACK_END;

    init_mark_stack(objspace);

    th->vm->self ? rb_gc_mark(th->vm->self) : rb_vm_mark(th->vm);

    mark_tbl(objspace, finalizer_table, 0);
    mark_current_machine_context(objspace, th);

    rb_gc_mark_symbols();
    rb_gc_mark_encodings();

    /* mark protected global variables */
    for (list = global_List; list; list = list->next) {
	rb_gc_mark_maybe(*list->varptr);
    }
    rb_mark_end_proc();
    rb_gc_mark_global_tbl();

    mark_tbl(objspace, rb_class_tbl, 0);

    /* mark generic instance variables for special constants */
    rb_mark_generic_ivar_tbl();

    rb_gc_mark_parser();

    rb_gc_mark_unlinked_live_method_entries(th->vm);

    /* gc_mark objects whose marking are not completed*/
    while (!MARK_STACK_EMPTY) {
	if (mark_stack_overflow) {
	    gc_mark_all(objspace);
	}
	else {
	    gc_mark_rest(objspace);
	}
    }
    GC_PROF_MARK_TIMER_STOP;
}

static int
garbage_collect(rb_objspace_t *objspace, int mark_only)
{
    INIT_GC_PROF_PARAMS;

    if (GC_NOTIFY) printf("start garbage_collect()\n");

    if (!heaps) {
	return FALSE;
    }
    if (!ready_to_gc(objspace)) {
        return TRUE;
    }

    GC_PROF_TIMER_START;

    rest_sweep(objspace);

    during_gc++;
    gc_marks(objspace);

    if (!mark_only) {
        GC_PROF_SWEEP_TIMER_START;
        gc_sweep(objspace);
        GC_PROF_SWEEP_TIMER_STOP;
    } else {
        before_gc_sweep(objspace);
        during_gc = 0;
    }

    GC_PROF_TIMER_STOP(Qtrue);
    if (GC_NOTIFY) printf("end garbage_collect()\n");
    return TRUE;
}

int
rb_garbage_collect(void)
{
    return garbage_collect(&rb_objspace, 0);
}

void
rb_gc_mark_machine_stack(rb_thread_t *th)
{
    rb_objspace_t *objspace = &rb_objspace;
    VALUE *stack_start, *stack_end;

    GET_STACK_BOUNDS(stack_start, stack_end, 0);
    rb_gc_mark_locations(stack_start, stack_end);
#ifdef __ia64
    rb_gc_mark_locations(th->machine_register_stack_start, th->machine_register_stack_end);
#endif
}


/*
 *  call-seq:
 *     GC.start                     -> nil
 *     gc.garbage_collect           -> nil
 *     ObjectSpace.garbage_collect  -> nil
 *
 *  Initiates garbage collection, unless manually disabled.
 *
 */

VALUE
rb_gc_start(void)
{
    rb_gc();
    return Qnil;
}

VALUE
rb_gc_mark_objects(void)
{
    rb_gc_mark_only();
    return Qnil;
}

#undef Init_stack

void
Init_stack(volatile VALUE *addr)
{
    ruby_init_stack(addr);
}

/*
 * Document-class: ObjectSpace
 *
 *  The <code>ObjectSpace</code> module contains a number of routines
 *  that interact with the garbage collection facility and allow you to
 *  traverse all living objects with an iterator.
 *
 *  <code>ObjectSpace</code> also provides support for object
 *  finalizers, procs that will be called when a specific object is
 *  about to be destroyed by garbage collection.
 *
 *     include ObjectSpace
 *
 *
 *     a = "A"
 *     b = "B"
 *     c = "C"
 *
 *
 *     define_finalizer(a, proc {|id| puts "Finalizer one on #{id}" })
 *     define_finalizer(a, proc {|id| puts "Finalizer two on #{id}" })
 *     define_finalizer(b, proc {|id| puts "Finalizer three on #{id}" })
 *
 *  <em>produces:</em>
 *
 *     Finalizer three on 537763470
 *     Finalizer one on 537763480
 *     Finalizer two on 537763480
 *
 */

void
Init_heap(void)
{
    init_heap(&rb_objspace);
}

static VALUE
lazy_sweep_enable(void)
{
    rb_objspace_t *objspace = &rb_objspace;

    objspace->flags.dont_lazy_sweep = FALSE;
    return Qnil;
}

typedef int each_obj_callback(void *, void *, size_t, void *);

struct each_obj_args {
    each_obj_callback *callback;
    void *data;
};

static VALUE
objspace_each_objects(VALUE arg)
{
    size_t i;
    struct heaps_header *membase = 0;
    RVALUE *pstart, *pend;
    rb_objspace_t *objspace = &rb_objspace;
    struct each_obj_args *args = (struct each_obj_args *)arg;
    volatile VALUE v;

    i = 0;
    while (i < heaps_used) {
	while (0 < i && membase < objspace->heap.sorted[i-1])
	    i--;
	while (i < heaps_used && objspace->heap.sorted[i] <= membase)
	    i++;
	if (heaps_used <= i)
	  break;
	membase = objspace->heap.sorted[i];

	pstart = membase->start;
	pend = membase->end;

	for (; pstart != pend; pstart++) {
	    if (pstart->as.basic.flags) {
		v = (VALUE)pstart; /* acquire to save this object */
		break;
	    }
	}
	if (pstart != pend) {
	    if ((*args->callback)(pstart, pend, sizeof(RVALUE), args->data)) {
		break;
	    }
	}
    }
    RB_GC_GUARD(v);

    return Qnil;
}

/*
 * rb_objspace_each_objects() is special C API to walk through
 * Ruby object space.  This C API is too difficult to use it.
 * To be frank, you should not use it. Or you need to read the
 * source code of this function and understand what this function does.
 *
 * 'callback' will be called several times (the number of heap slot,
 * at current implementation) with:
 *   vstart: a pointer to the first living object of the heap_slot.
 *   vend: a pointer to next to the valid heap_slot area.
 *   stride: a distance to next VALUE.
 *
 * If callback() returns non-zero, the iteration will be stopped.
 *
 * This is a sample callback code to iterate liveness objects:
 *
 *   int
 *   sample_callback(void *vstart, void *vend, int stride, void *data) {
 *     VALUE v = (VALUE)vstart;
 *     for (; v != (VALUE)vend; v += stride) {
 *       if (RBASIC(v)->flags) { // liveness check
 *       // do something with live object 'v'
 *     }
 *     return 0; // continue to iteration
 *   }
 *
 * Note: 'vstart' is not a top of heap_slot.  This point the first
 *       living object to grasp at least one object to avoid GC issue.
 *       This means that you can not walk through all Ruby object slot
 *       including freed object slot.
 *
 * Note: On this implementation, 'stride' is same as sizeof(RVALUE).
 *       However, there are possibilities to pass variable values with
 *       'stride' with some reasons.  You must use stride instead of
 *       use some constant value in the iteration.
 */
void
rb_objspace_each_objects(each_obj_callback *callback, void *data)
{
    struct each_obj_args args;
    rb_objspace_t *objspace = &rb_objspace;

    rest_sweep(objspace);
    objspace->flags.dont_lazy_sweep = TRUE;

    args.callback = callback;
    args.data = data;
    rb_ensure(objspace_each_objects, (VALUE)&args, lazy_sweep_enable, Qnil);
}

struct os_each_struct {
    size_t num;
    VALUE of;
};

static int
os_obj_of_i(void *vstart, void *vend, size_t stride, void *data)
{
    struct os_each_struct *oes = (struct os_each_struct *)data;
    RVALUE *p = (RVALUE *)vstart, *pend = (RVALUE *)vend;
    volatile VALUE v;

    for (; p != pend; p++) {
	if (p->as.basic.flags) {
	    switch (BUILTIN_TYPE(p)) {
	      case T_NONE:
	      case T_ICLASS:
	      case T_NODE:
	      case T_ZOMBIE:
		continue;
	      case T_CLASS:
		if (FL_TEST(p, FL_SINGLETON))
		  continue;
	      default:
		if (!p->as.basic.klass) continue;
		v = (VALUE)p;
		if (!oes->of || rb_obj_is_kind_of(v, oes->of)) {
		    rb_yield(v);
		    oes->num++;
		}
	    }
	}
    }

    return 0;
}

static VALUE
os_obj_of(VALUE of)
{
    struct os_each_struct oes;

    oes.num = 0;
    oes.of = of;
    rb_objspace_each_objects(os_obj_of_i, &oes);
    return SIZET2NUM(oes.num);
}

/*
 *  call-seq:
 *     ObjectSpace.each_object([module]) {|obj| ... } -> fixnum
 *     ObjectSpace.each_object([module])              -> an_enumerator
 *
 *  Calls the block once for each living, nonimmediate object in this
 *  Ruby process. If <i>module</i> is specified, calls the block
 *  for only those classes or modules that match (or are a subclass of)
 *  <i>module</i>. Returns the number of objects found. Immediate
 *  objects (<code>Fixnum</code>s, <code>Symbol</code>s
 *  <code>true</code>, <code>false</code>, and <code>nil</code>) are
 *  never returned. In the example below, <code>each_object</code>
 *  returns both the numbers we defined and several constants defined in
 *  the <code>Math</code> module.
 *
 *  If no block is given, an enumerator is returned instead.
 *
 *     a = 102.7
 *     b = 95       # Won't be returned
 *     c = 12345678987654321
 *     count = ObjectSpace.each_object(Numeric) {|x| p x }
 *     puts "Total count: #{count}"
 *
 *  <em>produces:</em>
 *
 *     12345678987654321
 *     102.7
 *     2.71828182845905
 *     3.14159265358979
 *     2.22044604925031e-16
 *     1.7976931348623157e+308
 *     2.2250738585072e-308
 *     Total count: 7
 *
 */

static VALUE
os_each_obj(int argc, VALUE *argv, VALUE os)
{
    VALUE of;

    rb_secure(4);
    if (argc == 0) {
	of = 0;
    }
    else {
	rb_scan_args(argc, argv, "01", &of);
    }
    RETURN_ENUMERATOR(os, 1, &of);
    return os_obj_of(of);
}

/* call-seq:
 *  ObjectSpace.live_objects => number
 */
static
VALUE os_live_objects(VALUE self)
{
    rb_objspace_t *objspace = &rb_objspace;
    return SIZET2NUM(objspace_live_num(objspace));
}

/*
 *  ObjectSpace.free_slots => number
 *
 * Returns the count of free slots available for new objects.
 */
static
VALUE os_free_slots(VALUE self)
{
    rb_objspace_t *objspace = &rb_objspace;
    size_t total = heaps_used * HEAP_OBJ_LIMIT, live = objspace_live_num(objspace);
    return SIZET2NUM(live>total ? 0 : total-live);
}

/* call-seq:
 *  ObjectSpace.allocated_objects => number
 *
 * Returns the count of objects allocated since the Ruby interpreter has
 * started.  This number can only increase. To know how many objects are
 * currently allocated, use ObjectSpace::live_objects
 */
static
VALUE os_allocated_objects(VALUE self)
{
    rb_objspace_t *objspace = &rb_objspace;
    return SIZET2NUM(objspace->total_allocated_object_num);
}

size_t
rb_os_allocated_objects(void)
{
  rb_objspace_t *objspace = &rb_objspace;
  return objspace->total_allocated_object_num;
}

/*
 *  call-seq:
 *     ObjectSpace.undefine_finalizer(obj)
 *
 *  Removes all finalizers for <i>obj</i>.
 *
 */

static VALUE
undefine_final(VALUE os, VALUE obj)
{
    rb_objspace_t *objspace = &rb_objspace;
    st_data_t data = obj;
    rb_check_frozen(obj);
    st_delete(finalizer_table, &data, 0);
    FL_UNSET(obj, FL_FINALIZE);
    return obj;
}

/*
 *  call-seq:
 *     ObjectSpace.define_finalizer(obj, aProc=proc())
 *
 *  Adds <i>aProc</i> as a finalizer, to be called after <i>obj</i>
 *  was destroyed.
 *
 */

static VALUE
define_final(int argc, VALUE *argv, VALUE os)
{
    rb_objspace_t *objspace = &rb_objspace;
    VALUE obj, block, table;
    st_data_t data;

    rb_scan_args(argc, argv, "11", &obj, &block);
    rb_check_frozen(obj);
    if (argc == 1) {
	block = rb_block_proc();
    }
    else if (!rb_respond_to(block, rb_intern("call"))) {
	rb_raise(rb_eArgError, "wrong type argument %s (should be callable)",
		 rb_obj_classname(block));
    }
    if (!FL_ABLE(obj)) {
	rb_raise(rb_eArgError, "cannot define finalizer for %s",
		 rb_obj_classname(obj));
    }
    RBASIC(obj)->flags |= FL_FINALIZE;

    block = rb_ary_new3(2, INT2FIX(rb_safe_level()), block);
    OBJ_FREEZE(block);

    if (st_lookup(finalizer_table, obj, &data)) {
	table = (VALUE)data;
	rb_ary_push(table, block);
    }
    else {
	table = rb_ary_new3(1, block);
	RBASIC(table)->klass = 0;
	st_add_direct(finalizer_table, obj, table);
    }
    return block;
}

void
rb_gc_copy_finalizer(VALUE dest, VALUE obj)
{
    rb_objspace_t *objspace = &rb_objspace;
    VALUE table;
    st_data_t data;

    if (!FL_TEST(obj, FL_FINALIZE)) return;
    if (st_lookup(finalizer_table, obj, &data)) {
	table = (VALUE)data;
	st_insert(finalizer_table, dest, table);
    }
    FL_SET(dest, FL_FINALIZE);
}

static VALUE
run_single_final(VALUE arg)
{
    VALUE *args = (VALUE *)arg;
    rb_eval_cmd(args[0], args[1], (int)args[2]);
    return Qnil;
}

static void
run_finalizer(rb_objspace_t *objspace, VALUE obj, VALUE table)
{
    long i;
    int status;
    VALUE args[3];
    VALUE objid = nonspecial_obj_id(obj);

    if (RARRAY_LEN(table) > 0) {
	args[1] = rb_obj_freeze(rb_ary_new3(1, objid));
    }
    else {
	args[1] = 0;
    }

    args[2] = (VALUE)rb_safe_level();
    for (i=0; i<RARRAY_LEN(table); i++) {
	VALUE final = RARRAY_PTR(table)[i];
	args[0] = RARRAY_PTR(final)[1];
	args[2] = FIX2INT(RARRAY_PTR(final)[0]);
	status = 0;
	rb_protect(run_single_final, (VALUE)args, &status);
	if (status)
	    rb_set_errinfo(Qnil);
    }
}

static void
run_final(rb_objspace_t *objspace, VALUE obj)
{
    RUBY_DATA_FUNC free_func = 0;
    st_data_t key, table;

    objspace->heap.final_num--;

    RBASIC(obj)->klass = 0;

    if (RTYPEDDATA_P(obj)) {
	free_func = RTYPEDDATA_TYPE(obj)->function.dfree;
    }
    else {
	free_func = RDATA(obj)->dfree;
    }
    if (free_func) {
	(*free_func)(DATA_PTR(obj));
    }

    key = (st_data_t)obj;
    if (st_delete(finalizer_table, &key, &table)) {
	run_finalizer(objspace, obj, (VALUE)table);
    }
}

static void
finalize_deferred(rb_objspace_t *objspace)
{
    RVALUE *p = deferred_final_list;
    deferred_final_list = 0;

    if (p) {
	finalize_list(objspace, p);
    }
}

void
rb_gc_finalize_deferred(void)
{
    rb_objspace_t *objspace = &rb_objspace;
    if (ATOMIC_EXCHANGE(finalizing, 1)) return;
    finalize_deferred(objspace);
    ATOMIC_SET(finalizing, 0);
}

static int
chain_finalized_object(st_data_t key, st_data_t val, st_data_t arg)
{
    RVALUE *p = (RVALUE *)key, **final_list = (RVALUE **)arg;
    if ((p->as.basic.flags & FL_FINALIZE) == FL_FINALIZE &&
        !MARKED_IN_BITMAP(GET_HEAP_BITMAP(p), p)) {
	if (BUILTIN_TYPE(p) != T_ZOMBIE) {
	    p->as.free.flags = T_ZOMBIE;
	    RDATA(p)->dfree = 0;
	}
	p->as.free.next = *final_list;
	*final_list = p;
    }
    return ST_CONTINUE;
}

struct force_finalize_list {
    VALUE obj;
    VALUE table;
    struct force_finalize_list *next;
};

static int
force_chain_object(st_data_t key, st_data_t val, st_data_t arg)
{
    struct force_finalize_list **prev = (struct force_finalize_list **)arg;
    struct force_finalize_list *curr = ALLOC(struct force_finalize_list);
    curr->obj = key;
    curr->table = val;
    curr->next = *prev;
    *prev = curr;
    return ST_CONTINUE;
}

void
rb_gc_call_finalizer_at_exit(void)
{
    rb_objspace_call_finalizer(&rb_objspace);
}

static void
rb_objspace_call_finalizer(rb_objspace_t *objspace)
{
    RVALUE *p, *pend;
    RVALUE *final_list = 0;
    size_t i;

    /* run finalizers */
    rest_sweep(objspace);

    if (ATOMIC_EXCHANGE(finalizing, 1)) return;

    do {
	/* XXX: this loop will make no sense */
	/* because mark will not be removed */
	finalize_deferred(objspace);
	mark_tbl(objspace, finalizer_table, 0);
	st_foreach(finalizer_table, chain_finalized_object,
		   (st_data_t)&deferred_final_list);
    } while (deferred_final_list);
    /* force to run finalizer */
    while (finalizer_table->num_entries) {
	struct force_finalize_list *list = 0;
	st_foreach(finalizer_table, force_chain_object, (st_data_t)&list);
	while (list) {
	    struct force_finalize_list *curr = list;
	    st_data_t obj = (st_data_t)curr->obj;
	    run_finalizer(objspace, curr->obj, curr->table);
	    st_delete(finalizer_table, &obj, 0);
	    list = curr->next;
	    xfree(curr);
	}
    }

    /* finalizers are part of garbage collection */
    during_gc++;

    /* run data object's finalizers */
    for (i = 0; i < heaps_used; i++) {
	p = objspace->heap.sorted[i]->start; pend = objspace->heap.sorted[i]->end;
	while (p < pend) {
	    if (BUILTIN_TYPE(p) == T_DATA &&
		DATA_PTR(p) && RANY(p)->as.data.dfree &&
		!rb_obj_is_thread((VALUE)p) && !rb_obj_is_mutex((VALUE)p) &&
		!rb_obj_is_fiber((VALUE)p)) {
		p->as.free.flags = 0;
		if (RTYPEDDATA_P(p)) {
		    RDATA(p)->dfree = RANY(p)->as.typeddata.type->function.dfree;
		}
		if (RANY(p)->as.data.dfree == (RUBY_DATA_FUNC)-1) {
		    xfree(DATA_PTR(p));
		}
		else if (RANY(p)->as.data.dfree) {
		    make_deferred(RANY(p));
		    RANY(p)->as.free.next = final_list;
		    final_list = p;
		}
	    }
	    else if (BUILTIN_TYPE(p) == T_FILE) {
		if (RANY(p)->as.file.fptr) {
		    make_io_deferred(RANY(p));
		    RANY(p)->as.free.next = final_list;
		    final_list = p;
		}
	    }
	    p++;
	}
    }
    during_gc = 0;
    if (final_list) {
	finalize_list(objspace, final_list);
    }

    st_free_table(finalizer_table);
    finalizer_table = 0;
    ATOMIC_SET(finalizing, 0);
}

void
rb_gc(void)
{
    rb_objspace_t *objspace = &rb_objspace;
    garbage_collect(objspace, 0);
    if (!finalizing) finalize_deferred(objspace);
}

void rb_gc_mark_only(void)
{
    rb_objspace_t *objspace = &rb_objspace;
    garbage_collect(objspace, 1);
    if (!finalizing) finalize_deferred(objspace);
}

static inline int
is_id_value(rb_objspace_t *objspace, VALUE ptr)
{
    if (!is_pointer_to_heap(objspace, (void *)ptr)) return FALSE;
    if (BUILTIN_TYPE(ptr) > T_FIXNUM) return FALSE;
    if (BUILTIN_TYPE(ptr) == T_ICLASS) return FALSE;
    return TRUE;
}

static inline int
is_dead_object(rb_objspace_t *objspace, VALUE ptr)
{
    struct heaps_slot *slot = objspace->heap.sweep_slots;
    if (!is_lazy_sweeping(objspace) || MARKED_IN_BITMAP(GET_HEAP_BITMAP(ptr), ptr))
	return FALSE;
    while (slot) {
	if ((VALUE)slot->membase->start <= ptr && ptr < (VALUE)(slot->membase->end))
	    return TRUE;
	slot = slot->next;
    }
    return FALSE;
}

static inline int
is_live_object(rb_objspace_t *objspace, VALUE ptr)
{
    if (BUILTIN_TYPE(ptr) == 0) return FALSE;
    if (RBASIC(ptr)->klass == 0) return FALSE;
    if (is_dead_object(objspace, ptr)) return FALSE;
    return TRUE;
}

/*
 *  call-seq:
 *     ObjectSpace._id2ref(object_id) -> an_object
 *
 *  Converts an object id to a reference to the object. May not be
 *  called on an object id passed as a parameter to a finalizer.
 *
 *     s = "I am a string"                    #=> "I am a string"
 *     r = ObjectSpace._id2ref(s.object_id)   #=> "I am a string"
 *     r == s                                 #=> true
 *
 */

static VALUE
id2ref(VALUE obj, VALUE objid)
{
#if SIZEOF_LONG == SIZEOF_VOIDP
#define NUM2PTR(x) NUM2ULONG(x)
#elif SIZEOF_LONG_LONG == SIZEOF_VOIDP
#define NUM2PTR(x) NUM2ULL(x)
#endif
    rb_objspace_t *objspace = &rb_objspace;
    VALUE ptr;
    void *p0;

    rb_secure(4);
    ptr = NUM2PTR(objid);
    p0 = (void *)ptr;

    if (ptr == Qtrue) return Qtrue;
    if (ptr == Qfalse) return Qfalse;
    if (ptr == Qnil) return Qnil;
    if (FIXNUM_P(ptr)) return (VALUE)ptr;
    ptr = objid ^ FIXNUM_FLAG;	/* unset FIXNUM_FLAG */

    if ((ptr % sizeof(RVALUE)) == (4 << 2)) {
        ID symid = ptr / sizeof(RVALUE);
        if (rb_id2name(symid) == 0)
	    rb_raise(rb_eRangeError, "%p is not symbol id value", p0);
	return ID2SYM(symid);
    }

    if (!is_id_value(objspace, ptr)) {
	rb_raise(rb_eRangeError, "%p is not id value", p0);
    }
    if (!is_live_object(objspace, ptr)) {
	rb_raise(rb_eRangeError, "%p is recycled object", p0);
    }
    return (VALUE)ptr;
}

/*
 *  Document-method: __id__
 *  Document-method: object_id
 *
 *  call-seq:
 *     obj.__id__       -> fixnum
 *     obj.object_id    -> fixnum
 *
 *  Returns an integer identifier for <i>obj</i>. The same number will
 *  be returned on all calls to <code>id</code> for a given object, and
 *  no two active objects will share an id.
 *  <code>Object#object_id</code> is a different concept from the
 *  <code>:name</code> notation, which returns the symbol id of
 *  <code>name</code>. Replaces the deprecated <code>Object#id</code>.
 */

/*
 *  call-seq:
 *     obj.hash    -> fixnum
 *
 *  Generates a <code>Fixnum</code> hash value for this object. This
 *  function must have the property that <code>a.eql?(b)</code> implies
 *  <code>a.hash == b.hash</code>. The hash value is used by class
 *  <code>Hash</code>. Any hash value that exceeds the capacity of a
 *  <code>Fixnum</code> will be truncated before being used.
 */

VALUE
rb_obj_id(VALUE obj)
{
    /*
     *                32-bit VALUE space
     *          MSB ------------------------ LSB
     *  false   00000000000000000000000000000000
     *  true    00000000000000000000000000000010
     *  nil     00000000000000000000000000000100
     *  undef   00000000000000000000000000000110
     *  symbol  ssssssssssssssssssssssss00001110
     *  object  oooooooooooooooooooooooooooooo00        = 0 (mod sizeof(RVALUE))
     *  fixnum  fffffffffffffffffffffffffffffff1
     *
     *                    object_id space
     *                                       LSB
     *  false   00000000000000000000000000000000
     *  true    00000000000000000000000000000010
     *  nil     00000000000000000000000000000100
     *  undef   00000000000000000000000000000110
     *  symbol   000SSSSSSSSSSSSSSSSSSSSSSSSSSS0        S...S % A = 4 (S...S = s...s * A + 4)
     *  object   oooooooooooooooooooooooooooooo0        o...o % A = 0
     *  fixnum  fffffffffffffffffffffffffffffff1        bignum if required
     *
     *  where A = sizeof(RVALUE)/4
     *
     *  sizeof(RVALUE) is
     *  20 if 32-bit, double is 4-byte aligned
     *  24 if 32-bit, double is 8-byte aligned
     *  40 if 64-bit
     */
    if (SYMBOL_P(obj)) {
        return (SYM2ID(obj) * sizeof(RVALUE) + (4 << 2)) | FIXNUM_FLAG;
    }
    if (SPECIAL_CONST_P(obj)) {
        return LONG2NUM((SIGNED_VALUE)obj);
    }
    return nonspecial_obj_id(obj);
}

static int
set_zero(st_data_t key, st_data_t val, st_data_t arg)
{
    VALUE k = (VALUE)key;
    VALUE hash = (VALUE)arg;
    rb_hash_aset(hash, k, INT2FIX(0));
    return ST_CONTINUE;
}

/*
 *  call-seq:
 *     ObjectSpace.count_objects([result_hash]) -> hash
 *
 *  Counts objects for each type.
 *
 *  It returns a hash as:
 *  {:TOTAL=>10000, :FREE=>3011, :T_OBJECT=>6, :T_CLASS=>404, ...}
 *
 *  If the optional argument, result_hash, is given,
 *  it is overwritten and returned.
 *  This is intended to avoid probe effect.
 *
 *  The contents of the returned hash is implementation defined.
 *  It may be changed in future.
 *
 *  This method is not expected to work except C Ruby.
 *
 */

static VALUE
count_objects(int argc, VALUE *argv, VALUE os)
{
    rb_objspace_t *objspace = &rb_objspace;
    size_t counts[T_MASK+1];
    size_t freed = 0;
    size_t total = 0;
    size_t i;
    VALUE hash;

    if (rb_scan_args(argc, argv, "01", &hash) == 1) {
        if (!RB_TYPE_P(hash, T_HASH))
            rb_raise(rb_eTypeError, "non-hash given");
    }

    for (i = 0; i <= T_MASK; i++) {
        counts[i] = 0;
    }

    for (i = 0; i < heaps_used; i++) {
        RVALUE *p, *pend;

        p = objspace->heap.sorted[i]->start; pend = objspace->heap.sorted[i]->end;
        for (;p < pend; p++) {
            if (p->as.basic.flags) {
                counts[BUILTIN_TYPE(p)]++;
            }
            else {
                freed++;
            }
        }
        total += objspace->heap.sorted[i]->limit;
    }

    if (hash == Qnil) {
        hash = rb_hash_new();
    }
    else if (!RHASH_EMPTY_P(hash)) {
        st_foreach(RHASH_TBL(hash), set_zero, hash);
    }
    rb_hash_aset(hash, ID2SYM(rb_intern("TOTAL")), SIZET2NUM(total));
    rb_hash_aset(hash, ID2SYM(rb_intern("FREE")), SIZET2NUM(freed));

    for (i = 0; i <= T_MASK; i++) {
        VALUE type;
        switch (i) {
#define COUNT_TYPE(t) case (t): type = ID2SYM(rb_intern(#t)); break;
	    COUNT_TYPE(T_NONE);
	    COUNT_TYPE(T_OBJECT);
	    COUNT_TYPE(T_CLASS);
	    COUNT_TYPE(T_MODULE);
	    COUNT_TYPE(T_FLOAT);
	    COUNT_TYPE(T_STRING);
	    COUNT_TYPE(T_REGEXP);
	    COUNT_TYPE(T_ARRAY);
	    COUNT_TYPE(T_HASH);
	    COUNT_TYPE(T_STRUCT);
	    COUNT_TYPE(T_BIGNUM);
	    COUNT_TYPE(T_FILE);
	    COUNT_TYPE(T_DATA);
	    COUNT_TYPE(T_MATCH);
	    COUNT_TYPE(T_COMPLEX);
	    COUNT_TYPE(T_RATIONAL);
	    COUNT_TYPE(T_NIL);
	    COUNT_TYPE(T_TRUE);
	    COUNT_TYPE(T_FALSE);
	    COUNT_TYPE(T_SYMBOL);
	    COUNT_TYPE(T_FIXNUM);
	    COUNT_TYPE(T_UNDEF);
	    COUNT_TYPE(T_NODE);
	    COUNT_TYPE(T_ICLASS);
	    COUNT_TYPE(T_ZOMBIE);
#undef COUNT_TYPE
          default:              type = INT2NUM(i); break;
        }
        if (counts[i])
            rb_hash_aset(hash, type, SIZET2NUM(counts[i]));
    }

    return hash;
}

static inline rb_obj_metadata_t *
rb_obj_get_metadata(VALUE obj)
{
    struct heaps_header *heap;

    if (SPECIAL_CONST_P(obj))
        return NULL;

    heap = GET_HEAP_HEADER(obj);
    if (!heap->metadata)
        return NULL;

    return &heap->metadata[NUM_IN_SLOT(obj)];
}

static VALUE
rb_obj_sourcefile(VALUE obj)
{
    rb_obj_metadata_t *meta = rb_obj_get_metadata(obj);

    if (!track_metadata)
        rb_warn("__sourcefile__ requires --debug-objects");

    return meta ? meta->file : Qnil;
}

static VALUE
rb_obj_sourceline(VALUE obj)
{
    rb_obj_metadata_t *meta = rb_obj_get_metadata(obj);

    if (!track_metadata)
        rb_warn("__sourceline__ requires --debug-objects");

    return meta ? INT2FIX(meta->line) : Qnil;
}

/*
 *  call-seq:
 *     GC.count -> Integer
 *
 *  The number of times GC occurred.
 *
 *  It returns the number of times GC occurred since the process started.
 *
 */

static VALUE
gc_count(VALUE self)
{
    return UINT2NUM((&rb_objspace)->count);
}

/*
 *  call-seq:
 *     GC.stat -> Hash
 *
 *  Returns a Hash containing information about the GC.
 *
 *  The hash includes information about internal statistics about GC such as:
 *
 *    {
 *      :count          => 18,
 *      :heap_used      => 77,
 *      :heap_length    => 77,
 *      :heap_increment => 0,
 *      :heap_live_num  => 23287,
 *      :heap_free_num  => 8115,
 *      :heap_final_num => 0,
 *    }
 *
 *  The contents of the hash are implementation defined and may be changed in
 *  the future.
 *
 *  This method is only expected to work on C Ruby.
 *
 */

static VALUE
gc_stat(int argc, VALUE *argv, VALUE self)
{
    size_t total_slots, live_slots, free_slots, unmarked_slots;
    rb_objspace_t *objspace = &rb_objspace;
    VALUE hash;

    if (rb_scan_args(argc, argv, "01", &hash) == 1) {
        if (!RB_TYPE_P(hash, T_HASH))
            rb_raise(rb_eTypeError, "non-hash given");
    }

    if (hash == Qnil) {
        hash = rb_hash_new();
    }

    rb_hash_aset(hash, ID2SYM(rb_intern("count")), SIZET2NUM(objspace->count));
    rb_hash_aset(hash, ID2SYM(rb_intern("time")), SIZET2NUM((size_t)(objspace->gc_time * 1000)));

    /* implementation dependent counters */
    total_slots = heaps_used * HEAP_OBJ_LIMIT;
    live_slots = objspace_live_num(objspace);
    free_slots = total_slots - live_slots;
    unmarked_slots = total_slots - objspace->heap.marked_num;

    rb_hash_aset(hash, ID2SYM(rb_intern("heap_used")), SIZET2NUM(objspace->heap.used));
    rb_hash_aset(hash, ID2SYM(rb_intern("heap_length")), SIZET2NUM(objspace->heap.length));
    rb_hash_aset(hash, ID2SYM(rb_intern("heap_obj_limit")), SIZET2NUM(HEAP_OBJ_LIMIT));
    rb_hash_aset(hash, ID2SYM(rb_intern("heap_total_num")), SIZET2NUM(total_slots));
    rb_hash_aset(hash, ID2SYM(rb_intern("heap_live_num")), SIZET2NUM(live_slots));
    rb_hash_aset(hash, ID2SYM(rb_intern("heap_free_num")), SIZET2NUM(free_slots));
    rb_hash_aset(hash, ID2SYM(rb_intern("heap_final_num")), SIZET2NUM(objspace->heap.final_num));
    rb_hash_aset(hash, ID2SYM(rb_intern("heap_increment")), SIZET2NUM(objspace->heap.increment));
    rb_hash_aset(hash, ID2SYM(rb_intern("heap_decrement")), SIZET2NUM(objspace->heap.decrement));
    rb_hash_aset(hash, ID2SYM(rb_intern("heap_marked_num")), SIZET2NUM(objspace->heap.marked_num));
    rb_hash_aset(hash, ID2SYM(rb_intern("heap_unmarked_num")), SIZET2NUM(unmarked_slots));
    rb_hash_aset(hash, ID2SYM(rb_intern("heap_swept_num")), SIZET2NUM(objspace->heap.swept_num));
    rb_hash_aset(hash, ID2SYM(rb_intern("heap_swept_pct")), !is_lazy_sweeping(objspace) ? INT2FIX(100) : SIZET2NUM(objspace->heap.swept_num * 100 / unmarked_slots));
    rb_hash_aset(hash, ID2SYM(rb_intern("is_lazy_sweeping")), is_lazy_sweeping(objspace) ? Qtrue : Qfalse);
    rb_hash_aset(hash, ID2SYM(rb_intern("total_allocated_object")), SIZET2NUM(objspace->total_allocated_object_num));
    rb_hash_aset(hash, ID2SYM(rb_intern("total_freed_object")), SIZET2NUM(objspace->total_freed_object_num));
    return hash;
}


#if CALC_EXACT_MALLOC_SIZE
/*
 *  call-seq:
 *     GC.malloc_allocated_size -> Integer
 *
 *  The allocated size by malloc().
 *
 *  It returns the allocated size by malloc().
 */

static VALUE
gc_malloc_allocated_size(VALUE self)
{
    return UINT2NUM((&rb_objspace)->malloc_params.allocated_size);
}

/*
 *  call-seq:
 *     GC.malloc_allocations -> Integer
 *
 *  The number of allocated memory object by malloc().
 *
 *  It returns the number of allocated memory object by malloc().
 */

static VALUE
gc_malloc_allocations(VALUE self)
{
    return UINT2NUM((&rb_objspace)->malloc_params.allocations);
}
#endif

/*
 *  call-seq:
 *     GC::Profiler.raw_data -> [Hash, ...]
 *
 *  Returns an Array of individual raw profile data Hashes ordered
 *  from earliest to latest by <tt>:GC_INVOKE_TIME</tt>.  For example:
 *
 *    [{:GC_TIME=>1.3000000000000858e-05,
 *      :GC_INVOKE_TIME=>0.010634999999999999,
 *      :HEAP_USE_SIZE=>289640,
 *      :HEAP_TOTAL_SIZE=>588960,
 *      :HEAP_TOTAL_OBJECTS=>14724,
 *      :GC_IS_MARKED=>false},
 *      ...
 *    ]
 *
 *  The keys mean:
 *
 *  +:GC_TIME+:: Time taken for this run in milliseconds
 *  +:GC_INVOKE_TIME+:: Time the GC was invoked since startup in seconds
 *  +:HEAP_USE_SIZE+:: Bytes of heap used
 *  +:HEAP_TOTAL_SIZE+:: Size of heap in bytes
 *  +:HEAP_TOTAL_OBJECTS+:: Number of objects
 *  +:GC_IS_MARKED+:: Is the GC in the mark phase
 *
 */

static VALUE
gc_profile_record_get(void)
{
    VALUE prof;
    VALUE gc_profile = rb_ary_new();
    size_t i;
    rb_objspace_t *objspace = (&rb_objspace);

    if (!objspace->profile.run) {
	return Qnil;
    }

    for (i =0; i < objspace->profile.count; i++) {
	prof = rb_hash_new();
        rb_hash_aset(prof, ID2SYM(rb_intern("GC_TIME")), DBL2NUM(objspace->profile.record[i].gc_time));
        rb_hash_aset(prof, ID2SYM(rb_intern("GC_INVOKE_TIME")), DBL2NUM(objspace->profile.record[i].gc_invoke_time));
        rb_hash_aset(prof, ID2SYM(rb_intern("HEAP_USE_SIZE")), SIZET2NUM(objspace->profile.record[i].heap_use_size));
        rb_hash_aset(prof, ID2SYM(rb_intern("HEAP_TOTAL_SIZE")), SIZET2NUM(objspace->profile.record[i].heap_total_size));
        rb_hash_aset(prof, ID2SYM(rb_intern("HEAP_TOTAL_OBJECTS")), SIZET2NUM(objspace->profile.record[i].heap_total_objects));
        rb_hash_aset(prof, ID2SYM(rb_intern("GC_IS_MARKED")), objspace->profile.record[i].is_marked);
#if GC_PROFILE_MORE_DETAIL
        rb_hash_aset(prof, ID2SYM(rb_intern("GC_MARK_TIME")), DBL2NUM(objspace->profile.record[i].gc_mark_time));
        rb_hash_aset(prof, ID2SYM(rb_intern("GC_SWEEP_TIME")), DBL2NUM(objspace->profile.record[i].gc_sweep_time));
        rb_hash_aset(prof, ID2SYM(rb_intern("ALLOCATE_INCREASE")), SIZET2NUM(objspace->profile.record[i].allocate_increase));
        rb_hash_aset(prof, ID2SYM(rb_intern("ALLOCATE_LIMIT")), SIZET2NUM(objspace->profile.record[i].allocate_limit));
        rb_hash_aset(prof, ID2SYM(rb_intern("HEAP_USE_SLOTS")), SIZET2NUM(objspace->profile.record[i].heap_use_slots));
        rb_hash_aset(prof, ID2SYM(rb_intern("HEAP_LIVE_OBJECTS")), SIZET2NUM(objspace->profile.record[i].heap_live_objects));
        rb_hash_aset(prof, ID2SYM(rb_intern("HEAP_FREE_OBJECTS")), SIZET2NUM(objspace->profile.record[i].heap_free_objects));
        rb_hash_aset(prof, ID2SYM(rb_intern("HAVE_FINALIZE")), objspace->profile.record[i].have_finalize);
#endif
	rb_ary_push(gc_profile, prof);
    }

    return gc_profile;
}

/*
 *  call-seq:
 *     GC::Profiler.result -> String
 *
 *  Returns a profile data report such as:
 *
 *    GC 1 invokes.
 *    Index    Invoke Time(sec)       Use Size(byte)     Total Size(byte)         Total Object                    GC time(ms)
 *        1               0.012               159240               212940                10647         0.00000000000001530000
 */

static VALUE
gc_profile_result(void)
{
    rb_objspace_t *objspace = &rb_objspace;
    VALUE record;
    VALUE result;
    int i, index;

    record = gc_profile_record_get();
    if (objspace->profile.run && objspace->profile.count) {
	result = rb_sprintf("GC %d invokes.\n", NUM2INT(gc_count(0)));
        index = 1;
	rb_str_cat2(result, "Index    Invoke Time(sec)       Use Size(byte)     Total Size(byte)         Total Object                    GC Time(ms)\n");
	for (i = 0; i < (int)RARRAY_LEN(record); i++) {
	    VALUE r = RARRAY_PTR(record)[i];
#if !GC_PROFILE_MORE_DETAIL
            if (rb_hash_aref(r, ID2SYM(rb_intern("GC_IS_MARKED")))) {
#endif
	    rb_str_catf(result, "%5d %19.3f %20"PRIuSIZE" %20"PRIuSIZE" %20"PRIuSIZE" %30.20f\n",
			index++, NUM2DBL(rb_hash_aref(r, ID2SYM(rb_intern("GC_INVOKE_TIME")))),
			(size_t)NUM2SIZET(rb_hash_aref(r, ID2SYM(rb_intern("HEAP_USE_SIZE")))),
			(size_t)NUM2SIZET(rb_hash_aref(r, ID2SYM(rb_intern("HEAP_TOTAL_SIZE")))),
			(size_t)NUM2SIZET(rb_hash_aref(r, ID2SYM(rb_intern("HEAP_TOTAL_OBJECTS")))),
			NUM2DBL(rb_hash_aref(r, ID2SYM(rb_intern("GC_TIME"))))*1000);
#if !GC_PROFILE_MORE_DETAIL
            }
#endif
	}
#if GC_PROFILE_MORE_DETAIL
	rb_str_cat2(result, "\n\n");
	rb_str_cat2(result, "More detail.\n");
	rb_str_cat2(result, "Index Allocate Increase    Allocate Limit  Use Slot  Have Finalize             Mark Time(ms)            Sweep Time(ms)\n");
        index = 1;
	for (i = 0; i < (int)RARRAY_LEN(record); i++) {
	    VALUE r = RARRAY_PTR(record)[i];
	    rb_str_catf(result, "%5d %17"PRIuSIZE" %17"PRIuSIZE" %9"PRIuSIZE" %14s %25.20f %25.20f\n",
			index++, (size_t)NUM2SIZET(rb_hash_aref(r, ID2SYM(rb_intern("ALLOCATE_INCREASE")))),
			(size_t)NUM2SIZET(rb_hash_aref(r, ID2SYM(rb_intern("ALLOCATE_LIMIT")))),
			(size_t)NUM2SIZET(rb_hash_aref(r, ID2SYM(rb_intern("HEAP_USE_SLOTS")))),
			rb_hash_aref(r, ID2SYM(rb_intern("HAVE_FINALIZE")))? "true" : "false",
			NUM2DBL(rb_hash_aref(r, ID2SYM(rb_intern("GC_MARK_TIME"))))*1000,
			NUM2DBL(rb_hash_aref(r, ID2SYM(rb_intern("GC_SWEEP_TIME"))))*1000);
	}
#endif
    }
    else {
	result = rb_str_new2("");
    }
    return result;
}


/*
 *  call-seq:
 *     GC::Profiler.report
 *     GC::Profiler.report io
 *
 *  Writes the GC::Profiler#result to <tt>$stdout</tt> or the given IO object.
 *
 */

static VALUE
gc_profile_report(int argc, VALUE *argv, VALUE self)
{
    VALUE out;

    if (argc == 0) {
	out = rb_stdout;
    }
    else {
	rb_scan_args(argc, argv, "01", &out);
    }
    rb_io_write(out, gc_profile_result());

    return Qnil;
}

/*
 *  call-seq:
 *     GC::Profiler.total_time -> float
 *
 *  The total time used for garbage collection in milliseconds
 */

static VALUE
gc_profile_total_time(VALUE self)
{
    double time = 0;
    rb_objspace_t *objspace = &rb_objspace;
    size_t i;

    if (objspace->profile.run && objspace->profile.count) {
	for (i = 0; i < objspace->profile.count; i++) {
	    time += objspace->profile.record[i].gc_time;
	}
    }
    return DBL2NUM(time);
}

// GC::BasicProfiler methods

/*
 *  call-seq:
 *    GC::BasicProfiler.enabled?                 -> true or false
 *
 *  The current status of GC::BasicProfiler.
 */

static VALUE
gc_basic_profile_enable_get(VALUE self)
{
    rb_objspace_t *objspace = &rb_objspace;
    return INT2BOOL(objspace->basic_profile.run);
}

/*
 *  call-seq:
 *    GC::BasicProfiler.enable          -> nil
 *
 *  Starts the Basic GC profiler.
 *
 */

static VALUE
gc_basic_profile_enable(void)
{
    rb_objspace_t *objspace = &rb_objspace;
    objspace->basic_profile.run = TRUE;

    return Qnil;
}

/*
 *  call-seq:
 *    GC::BasicProfiler.disable          -> nil
 *
 *  Stops the Basic GC profiler.
 *
 */

static VALUE
gc_basic_profile_disable(void)
{
    rb_objspace_t *objspace = &rb_objspace;

    MEMZERO(&objspace->basic_profile, struct rb_gc_basic_profile, 1);

    return Qnil;
}

/*
 *  call-seq:
 *     GC::BasicProfiler.total_time -> float
 *
 *  The total time used for garbage collection in milliseconds
 */

static VALUE
gc_basic_profile_total_time(VALUE self)
{
    rb_objspace_t *objspace = &rb_objspace;

    return DBL2NUM(objspace->basic_profile.total_time);
}

/*
 *  call-seq:
 *     GC::BasicProfiler.count -> int
 *
 *  The number of times the gc has run
 */

static VALUE
gc_basic_profile_count(VALUE self)
{
    rb_objspace_t *objspace = &rb_objspace;

    return SIZET2NUM(objspace->basic_profile.count);
}

/*  Document-class: GC::Profiler
 *
 *  The GC profiler provides access to information on GC runs including time,
 *  length and object space size.
 *
 *  Example:
 *
 *    GC::Profiler.enable
 *
 *    require 'rdoc/rdoc'
 *
 *    puts GC::Profiler.result
 *
 *    GC::Profiler.disable
 *
 *  See also GC.count, GC.malloc_allocated_size and GC.malloc_allocations
 */

/*
 *  The <code>GC</code> module provides an interface to Ruby's mark and
 *  sweep garbage collection mechanism. Some of the underlying methods
 *  are also available via the ObjectSpace module.
 *
 *  You may obtain information about the operation of the GC through
 *  GC::Profiler.
 */

void
Init_GC(void)
{
    VALUE rb_mObSpace;
    VALUE rb_mProfiler;
    VALUE rb_mBasicProfiler;

    rb_mGC = rb_define_module("GC");
    rb_define_singleton_method(rb_mGC, "start", rb_gc_start, 0);
    rb_define_singleton_method(rb_mGC, "mark_objects", rb_gc_mark_objects, 0);
    rb_define_singleton_method(rb_mGC, "enable", rb_gc_enable, 0);
    rb_define_singleton_method(rb_mGC, "disable", rb_gc_disable, 0);
    rb_define_singleton_method(rb_mGC, "stress", gc_stress_get, 0);
    rb_define_singleton_method(rb_mGC, "stress=", gc_stress_set, 1);
    rb_define_singleton_method(rb_mGC, "count", gc_count, 0);
    rb_define_singleton_method(rb_mGC, "stat", gc_stat, -1);
    rb_define_method(rb_mGC, "garbage_collect", rb_gc_start, 0);

    rb_mProfiler = rb_define_module_under(rb_mGC, "Profiler");
    rb_define_singleton_method(rb_mProfiler, "enabled?", gc_profile_enable_get, 0);
    rb_define_singleton_method(rb_mProfiler, "enable", gc_profile_enable, 0);
    rb_define_singleton_method(rb_mProfiler, "raw_data", gc_profile_record_get, 0);
    rb_define_singleton_method(rb_mProfiler, "disable", gc_profile_disable, 0);
    rb_define_singleton_method(rb_mProfiler, "clear", gc_profile_clear, 0);
    rb_define_singleton_method(rb_mProfiler, "result", gc_profile_result, 0);
    rb_define_singleton_method(rb_mProfiler, "report", gc_profile_report, -1);
    rb_define_singleton_method(rb_mProfiler, "total_time", gc_profile_total_time, 0);

    rb_mBasicProfiler = rb_define_module_under(rb_mGC, "BasicProfiler");
    rb_define_singleton_method(rb_mBasicProfiler, "enabled?", gc_basic_profile_enable_get, 0);
    rb_define_singleton_method(rb_mBasicProfiler, "enable", gc_basic_profile_enable, 0);
    rb_define_singleton_method(rb_mBasicProfiler, "disable", gc_basic_profile_disable, 0);
    rb_define_singleton_method(rb_mBasicProfiler, "total_time", gc_basic_profile_total_time, 0);
    rb_define_singleton_method(rb_mBasicProfiler, "count", gc_basic_profile_count, 0);

    rb_mObSpace = rb_define_module("ObjectSpace");
    rb_define_module_function(rb_mObSpace, "each_object", os_each_obj, -1);
    rb_define_module_function(rb_mObSpace, "garbage_collect", rb_gc_start, 0);
    rb_define_module_function(rb_mObSpace, "free_slots", os_free_slots, 0);
    rb_define_module_function(rb_mObSpace, "allocated_objects", os_allocated_objects, 0);
    rb_define_module_function(rb_mObSpace, "live_objects", os_live_objects, 0);

    rb_define_module_function(rb_mObSpace, "define_finalizer", define_final, -1);
    rb_define_module_function(rb_mObSpace, "undefine_finalizer", undefine_final, 1);

    rb_define_module_function(rb_mObSpace, "_id2ref", id2ref, 1);

    nomem_error = rb_exc_new3(rb_eNoMemError,
			      rb_obj_freeze(rb_str_new2("failed to allocate memory")));
    OBJ_TAINT(nomem_error);
    OBJ_FREEZE(nomem_error);

    rb_define_method(rb_cBasicObject, "__id__", rb_obj_id, 0);
    rb_define_method(rb_mKernel, "object_id", rb_obj_id, 0);

    rb_define_method(rb_cBasicObject, "__sourcefile__", rb_obj_sourcefile, 0);
    rb_define_method(rb_cBasicObject, "__sourceline__", rb_obj_sourceline, 0);

    rb_define_module_function(rb_mObSpace, "count_objects", count_objects, -1);

#if CALC_EXACT_MALLOC_SIZE
    rb_define_singleton_method(rb_mGC, "malloc_allocated_size", gc_malloc_allocated_size, 0);
    rb_define_singleton_method(rb_mGC, "malloc_allocations", gc_malloc_allocations, 0);
#endif
}
