/**
 * Memory pool benchmark.
 *
 * Build with:
 * $ g++ -O2 -std=c++11 -Wall -lboost_system pool.cc
 *
 * Copyright (C) 2015 Alexander Krizhanovsky (ak@natsys-lab.com).
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 * See http://www.gnu.org/licenses/lgpl.html .
 */
#include <assert.h>
#include <stdlib.h>

#include <chrono>
#include <functional>
#include <iomanip>
#include <iostream>
#include <string>

#include <boost/pool/object_pool.hpp>

static const size_t PAGE_SIZE = 4096;

// sizeof(TfwStr)
struct Small {
	long l[3];

	std::string
	operator+(const char *str)
	{ return std::string(str) + " (Small)"; }
};

// size of common HTTP headers table
struct Big {
	Small s[10];

	std::string
	operator+(const char *str)
	{ return std::string(str) + " (Big)"; }
};

// very big allocation
struct Huge {
	char c[PAGE_SIZE * 2 + 113];

	std::string
	operator+(const char *str)
	{ return std::string(str) + " (Huge)"; }
};

static const size_t N = 10 * 1000 * 1000;

void
benchmark(std::string &&desc, std::function<void ()> cb)
{
	using namespace std::chrono;

	// steady_clock has the same resolution as high_resolution_clock
	auto t(steady_clock::now());

	cb();

	auto dt = steady_clock::now() - t;
	std::cout << std::setw(30) << std::right << desc << ":    "
		  << duration_cast<milliseconds>(dt).count()
		  << "ms" << std::endl;
}

/*
 * ------------------------------------------------------------------------
 *	Boost::pool
 * ------------------------------------------------------------------------
 */
template<class T>
void
benchmark_boost_pool()
{
	boost::object_pool<T> p;

	benchmark(std::move(T() + "boost::pool"), [&]() {
		for (size_t i = 0; i < N * sizeof(Small) / sizeof(T); ++i) {
			T *o = p.malloc();
			memset(o, 1, sizeof(*o));
		}
	});
}

template<class T>
void
benchmark_boost_pool_free()
{
	boost::object_pool<T> p;

	benchmark(std::move(T() + "boost::pool w/ free"), [&]() {
		for (size_t i = 0; i < N * sizeof(Small) / sizeof(T); ++i) {
			T *o = p.malloc();
			memset(o, 1, sizeof(*o));
			if (__builtin_expect(!(i & 3), 0))
				p.free(o);
		}
	});
}

/*
 * ------------------------------------------------------------------------
 *	Nginx pool
 *	Simplified version of src/core/ngx_palloc.c from nginx-1.9.5 with
 *	some code adjustments to be built in this benchmark.
 * ------------------------------------------------------------------------
 */
struct ngx_pool_t;
typedef uintptr_t       ngx_uint_t;

#define NGX_POOL_ALIGNMENT	16
#define NGX_ALIGNMENT		sizeof(unsigned long)
#define ngx_align_ptr(p, a)						\
	(u_char *)(((uintptr_t)(p) + ((uintptr_t)a - 1)) & ~((uintptr_t)a - 1))
#define NGX_MAX_ALLOC_FROM_POOL	((size_t)PAGE_SIZE - 1)

struct ngx_pool_data_t {
	unsigned char	*last;
	unsigned char	*end;
	ngx_pool_t	*next;
	unsigned int	failed;
};

struct ngx_pool_t {
	ngx_pool_data_t	d;
	size_t		max;
	ngx_pool_t	*current;
};

ngx_pool_t *
ngx_create_pool(size_t size)
{
	ngx_pool_t *p;

	if (posix_memalign((void **)&p, NGX_POOL_ALIGNMENT, size))
		return NULL;

	p->d.last = (unsigned char *)p + sizeof(ngx_pool_t);
	p->d.end = (unsigned char *)p + size;
	p->d.next = NULL;
	p->d.failed = 0;

	size = size - sizeof(ngx_pool_t);
	p->max = (size < NGX_MAX_ALLOC_FROM_POOL)
		 ? size : NGX_MAX_ALLOC_FROM_POOL;

	p->current = p;

	return p;
}

void
ngx_destroy_pool(ngx_pool_t *pool)
{
	ngx_pool_t *p, *n;

	for (p = pool, n = pool->d.next; ; p = n, n = n->d.next) {
		free(p);
		if (n == NULL)
			break;
	}
}

static void *
ngx_palloc_block(ngx_pool_t *pool, size_t size)
{
	unsigned char *m;
	size_t psize = (size_t)(pool->d.end - (unsigned char *)pool);
	ngx_pool_t *p, *p_new;

	if (posix_memalign((void **)&m, NGX_POOL_ALIGNMENT, psize))
		return NULL;
	p_new = (ngx_pool_t *)m;

	p_new->d.end = m + psize;
	p_new->d.next = NULL;
	p_new->d.failed = 0;

	m += sizeof(ngx_pool_data_t);
	m = ngx_align_ptr(m, NGX_ALIGNMENT);
	p_new->d.last = m + size;

	for (p = pool->current; p->d.next; p = p->d.next) {
		if (p->d.failed++ > 4)
			pool->current = p->d.next;
	}
	p->d.next = p_new;

	return m;
}

void *
ngx_palloc(ngx_pool_t *pool, size_t size)
{
	unsigned char *m;
	ngx_pool_t *p = pool->current;

	do {
		m = ngx_align_ptr(p->d.last, NGX_ALIGNMENT);
		if ((size_t)(p->d.end - m) >= size) {
			p->d.last = m + size;
			return m;
		}
		p = p->d.next;
	} while (p);

        return ngx_palloc_block(pool, size);
}

template<class T>
void
benchmark_ngx_pool()
{
	ngx_pool_t *p = ngx_create_pool(PAGE_SIZE);
	assert(p);

	benchmark(std::move(T() + "ngx_pool"), [=]() {
		for (size_t i = 0; i < N * sizeof(Small) / sizeof(T); ++i) {
			T *o = (T *)ngx_palloc(p, sizeof(T));
			memset(o, 1, sizeof(*o));
		}
	});

	ngx_destroy_pool(p);
}

void
benchmark_ngx_pool_mix()
{
	ngx_pool_t *p = ngx_create_pool(PAGE_SIZE);
	assert(p);

	benchmark(std::move(std::string("ngx_pool (Mix)")), [=]() {
		for (size_t i = 0; i < N; ++i) {
			if (__builtin_expect(!(i & 3), 0)) {
				Big *o = (Big *)ngx_palloc(p, sizeof(Big));
				memset(o, 1, sizeof(*o));
			} else {
				Small *o = (Small *)ngx_palloc(p,
							       sizeof(Small));
				memset(o, 1, sizeof(*o));
			}
		}
	});

	ngx_destroy_pool(p);
}

/*
 * ------------------------------------------------------------------------
 *	Tempesta FW pool
 *	Simplified and ported to user-space from
 *	https://github.com/natsys/tempesta/blob/master/tempesta_fw/pool.c
 * ------------------------------------------------------------------------
 */
/**
 * Emulation of Linux buddy allocator.
 */
#define GFP_ATOMIC		0
#define PAGE_MASK		(~(PAGE_SIZE - 1))
#define unlikely(x)		__builtin_expect(x, 0)
#define free_pages(p, o)	free((void *)(p))
#define get_order(n)		(assert((n) < PAGE_SIZE * 128),		\
				 (n) < PAGE_SIZE ? 0			\
				 : (n) < PAGE_SIZE * 2 ? 1		\
				 : (n) < PAGE_SIZE * 4 ? 2		\
				 : (n) < PAGE_SIZE * 8 ? 3		\
				 : (n) < PAGE_SIZE * 16 ? 4		\
				 : (n) < PAGE_SIZE * 32 ? 5		\
				 : (n) < PAGE_SIZE * 64 ? 6		\
				 : 7)

static inline void *
__get_free_pages(int flags __attribute__((unused)), int order)
{
	void *p;

	if (posix_memalign((void **)&p, PAGE_SIZE, PAGE_SIZE << order))
		return NULL;

	return p;
}

typedef struct TfwPoolChunk TfwPoolChunk;

/**
 * Memory pool chunk descriptor.
 *
 * @next	- pointer to next memory chunk;
 * @prev	- pointer to previous memory chunk
 * 		  (used only for TfwPool->curr list);
 * @order	- order of number of pages in the chunk;
 * @off		- current chunk offset;
 */
struct TfwPoolChunk {
	TfwPoolChunk *next;
	TfwPoolChunk *prev;
	unsigned int order;
	unsigned int off;
};

/**
 * Memory pool descriptor.
 *
 * @curr	- current chunk to allocate memory from;
 * @free	- current of list of free chunks;
 */
typedef struct {
	TfwPoolChunk *curr;
	TfwPoolChunk *free;
} TfwPool;

#define TFW_POOL_CHUNK_SIZE(c)	(PAGE_SIZE << (c)->order)
#define TFW_POOL_CHUNK_BASE(c)	((unsigned long)(c) & PAGE_MASK)
#define TFW_POOL_CHUNK_OFF(c)	(TFW_POOL_CHUNK_BASE(c) + (c)->off)
#define TFW_POOL_ALIGN_SZ(n)	(((n) + 7) & ~7UL)

static inline TfwPoolChunk *
tfw_pool_chunk_first(TfwPool *p)
{
	return (TfwPoolChunk *)TFW_POOL_ALIGN_SZ((unsigned long)(p + 1));
}

static inline unsigned int
tfw_pool_chunk_startoff(TfwPool *p, TfwPoolChunk *c)
{
	return TFW_POOL_ALIGN_SZ((char *)(c + 1) - (char *)p);
}

/**
 * Allocate bit more pages than we need.
 */
TfwPool *
__tfw_pool_new(size_t n)
{
	TfwPool *p;
	TfwPoolChunk *c;
	unsigned int order;

	n = TFW_POOL_ALIGN_SZ(n);
	order = get_order(n + sizeof(*p) + sizeof(*c));

	p = (TfwPool *)__get_free_pages(GFP_ATOMIC, order);
	if (!p)
		return NULL;

	c = tfw_pool_chunk_first(p);
	c->off = tfw_pool_chunk_startoff(p, c);
	c->order = order;
	c->next = c->prev = NULL;

	p->curr = c;
	p->free = NULL;

	return p;
}

/**
 * Allocate pages from buddy allocator and put extra pages to
 * list of free pages.
 *
 * @n is already aligned.
 */
TfwPoolChunk *
tfw_pool_alloc_chunk(TfwPool *p, size_t n)
{
	unsigned int order = get_order(n);
	unsigned int free_pgs = (1 << order) - (n + PAGE_SIZE - 1) / PAGE_SIZE;
	TfwPoolChunk *chunk = p->free;

	if (unlikely(chunk && chunk->off + n <= TFW_POOL_CHUNK_SIZE(chunk))) {
		p->free = chunk->next;
		return chunk;
	}

	chunk = (TfwPoolChunk *)__get_free_pages(GFP_ATOMIC, order);
	if (!chunk)
		return NULL;

	chunk->order = order;
	chunk->off = TFW_POOL_ALIGN_SZ(sizeof(*chunk)) + n;
	chunk->next = p->curr;
	chunk->prev = NULL;
	p->curr->prev = chunk;
	p->curr = chunk;

	for ( ; free_pgs; --free_pgs) {
		unsigned int shift = ((1 << order) - free_pgs) * PAGE_SIZE;
		TfwPoolChunk *c = (TfwPoolChunk *)((char *)chunk + shift);
		c->order = 0; /* single pages only */
		c->off = TFW_POOL_ALIGN_SZ(sizeof(*c));
		c->next = p->free ? p->free : NULL;
		p->free = c;
	}

	return (TfwPoolChunk *)TFW_POOL_ALIGN_SZ((unsigned long)(chunk + 1));
}

void *
tfw_pool_alloc(TfwPool *p, size_t n)
{
	void *a;
	TfwPoolChunk *c = p->curr;

	n = TFW_POOL_ALIGN_SZ(n);

	/*
	 * If the chunk is larger than 2 pages, then don't use it for small
	 * allocations since we can't find begin of the chunk.
	 */
	if (unlikely(c->off + n > TFW_POOL_CHUNK_SIZE(c) || c->order))
		return tfw_pool_alloc_chunk(p, n);

	a = (char *)TFW_POOL_CHUNK_OFF(c);
	c->off += n;

	return a;
}

/**
 * It's good to call the function against just allocated chunk.
 */
void
tfw_pool_free(TfwPool *p, void *ptr, size_t n)
{
	TfwPoolChunk *c = (TfwPoolChunk *)TFW_POOL_CHUNK_BASE(ptr);

	if (unlikely((TfwPool *)c == p))
		c = tfw_pool_chunk_first(p);

	n = TFW_POOL_ALIGN_SZ(n);
	if ((char *)ptr + n == (char *)TFW_POOL_CHUNK_OFF(c)) {
		c->off -= n;
		if (p->curr != c && c->off == tfw_pool_chunk_startoff(p, c)) {
			/* Unlink from current list. */
			if (c->next)
				c->next->prev = c->prev;
			c->prev->next = c->next;
			/* Link to free list, @next only is used. */
			c->next = p->free;
			p->free = c;
		}
	}
}

void
tfw_pool_destroy(TfwPool *p)
{
	TfwPoolChunk *c, *next;

	for (c = p->curr; c; c = next) {
		next = c->next;
		if (c == tfw_pool_chunk_first(p))
			continue;
		free_pages(TFW_POOL_CHUNK_BASE(c), c->order);
	}
	for (c = p->free; c; c = next) {
		next = c->next;
		if (c == tfw_pool_chunk_first(p))
			continue;
		free_pages(TFW_POOL_CHUNK_BASE(c), c->order);
	}
	free_pages(p, tfw_pool_first_chunk(p)->order);
}

template<class T>
void
benchmark_tfw_pool()
{
	TfwPool *p = __tfw_pool_new(0);
	assert(p);

	benchmark(std::move(T() + "tfw_pool"), [=]() {
		for (size_t i = 0; i < N * sizeof(Small) / sizeof(T); ++i) {
			T *o = (T *)tfw_pool_alloc(p, sizeof(T));
			memset(o, 1, sizeof(*o));
		}
	});

	tfw_pool_destroy(p);
}

void
benchmark_tfw_pool_mix()
{
	TfwPool *p = __tfw_pool_new(0);
	assert(p);

	benchmark(std::move(std::string("tfw_pool (Mix)")), [=]() {
		for (size_t i = 0; i < N; ++i) {
			if (__builtin_expect(!(i & 3), 0)) {
				Big *o = (Big *)tfw_pool_alloc(p, sizeof(Big));
				memset(o, 1, sizeof(*o));
			} else {
				Small *o;
				o = (Small *)tfw_pool_alloc(p, sizeof(Small));
				memset(o, 1, sizeof(*o));
			}
		}
	});

	tfw_pool_destroy(p);
}

template<class T>
void
benchmark_tfw_pool_free()
{
	TfwPool *p = __tfw_pool_new(0);
	assert(p);

	benchmark(std::move(T() + "tfw_pool w/ free"), [&]() {
		for (size_t i = 0; i < N * sizeof(Small) / sizeof(T); ++i) {
			T *o = (T *)tfw_pool_alloc(p, sizeof(T));
			memset(o, 1, sizeof(*o));
			if (__builtin_expect(!(i & 3), 0))
				tfw_pool_free(p, o, sizeof(T));
		}
	});

	tfw_pool_destroy(p);
}


/*
 * ------------------------------------------------------------------------
 *	Main part of the benchmark
 * ------------------------------------------------------------------------
 */
int
main()
{
	std::cout << std::setw(35) << std::right << "small object size:    "
		  << sizeof(Small) << std::endl;
	std::cout << std::setw(35) << std::right << "big object size:    "
		  << sizeof(Big) << "\n" << std::endl;

	// Warm up malloc().
	static const size_t n = N * sizeof(Big);
	void *p = malloc(n);
	assert(p);
	memset(p, 0, n);
	free(p);
/*
	benchmark_boost_pool<Small>();
	benchmark_boost_pool_free<Small>();
	benchmark_boost_pool<Big>();
	benchmark_boost_pool_free<Big>();
	std::cout << std::endl;

	benchmark_ngx_pool<Small>();
	benchmark_ngx_pool<Big>();
	benchmark_ngx_pool_mix();
	std::cout << std::endl;
*/
	benchmark_tfw_pool<Small>();
	benchmark_tfw_pool<Big>();
	benchmark_tfw_pool<Huge>();
	benchmark_tfw_pool_mix();
	benchmark_tfw_pool_free<Small>();
	benchmark_tfw_pool_free<Big>();
	benchmark_tfw_pool_free<Huge>();

	return 0;
}
