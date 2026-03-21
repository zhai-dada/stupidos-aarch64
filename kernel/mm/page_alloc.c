#include "mm/page_alloc.h"

#include "mmu.h"
#include "mm/mm.h"
#include "printk.h"

#define PAGE_ALLOC_FLAG_FREE    0x0001

struct page_block
{
    int32_t next;
    int32_t prev;
    uint16_t order;
    uint16_t flags;
};

static struct page_block page_map[TOTAL_MEMORY / PAGE_SIZE];
static int32_t free_area[PAGE_ALLOC_MAX_ORDER + 1];
static uint32_t free_pages_count;
static uint64_t alloc_base_phys;

static inline uint32_t page_count(void)
{
    return (uint32_t)(TOTAL_MEMORY / PAGE_SIZE);
}

static inline uint64_t page_index_to_phys(uint32_t index)
{
    return alloc_base_phys + ((uint64_t)index * PAGE_SIZE);
}

static inline uint32_t phys_to_page_index(uint64_t phys)
{
    return (uint32_t)((phys - alloc_base_phys) / PAGE_SIZE);
}

static void free_list_push(uint32_t order, uint32_t index)
{
    int32_t head;
    struct page_block *page;

    page = &page_map[index];
    head = free_area[order];
    page->next = head;
    page->prev = -1;
    page->order = order;
    page->flags = PAGE_ALLOC_FLAG_FREE;

    if (head >= 0)
    {
        page_map[head].prev = (int32_t)index;
    }

    free_area[order] = (int32_t)index;
}

static void free_list_remove(uint32_t order, uint32_t index)
{
    struct page_block *page;

    page = &page_map[index];
    if (page->prev >= 0)
    {
        page_map[(uint32_t)page->prev].next = page->next;
    }
    else
    {
        free_area[order] = page->next;
    }

    if (page->next >= 0)
    {
        page_map[(uint32_t)page->next].prev = page->prev;
    }

    page->next = -1;
    page->prev = -1;
}

static uint32_t order_for_count(uint32_t count)
{
    uint32_t order;

    order = 0;
    while ((1U << order) < count && order < PAGE_ALLOC_MAX_ORDER)
    {
        order++;
    }

    return order;
}

static void free_block(uint32_t index, uint32_t order)
{
    uint32_t buddy;
    uint32_t cur_index;
    uint32_t cur_order;

    cur_index = index;
    cur_order = order;

    while (cur_order < PAGE_ALLOC_MAX_ORDER)
    {
        buddy = cur_index ^ (1U << cur_order);
        if (buddy >= page_count())
        {
            break;
        }

        if ((page_map[buddy].flags & PAGE_ALLOC_FLAG_FREE) == 0 ||
            page_map[buddy].order != cur_order)
        {
            break;
        }

        free_list_remove(cur_order, buddy);
        page_map[buddy].flags = 0;
        if (buddy < cur_index)
        {
            cur_index = buddy;
        }
        cur_order++;
    }

    free_list_push(cur_order, cur_index);
    free_pages_count += (1U << order);
}

static uint32_t alloc_block(uint32_t order)
{
    uint32_t cur_order;
    int32_t index;

    for (cur_order = order; cur_order <= PAGE_ALLOC_MAX_ORDER; cur_order++)
    {
        index = free_area[cur_order];
        if (index >= 0)
        {
            break;
        }
    }

    if (cur_order > PAGE_ALLOC_MAX_ORDER || index < 0)
    {
        return (uint32_t)-1;
    }

    free_list_remove(cur_order, (uint32_t)index);
    page_map[(uint32_t)index].flags = 0;

    while (cur_order > order)
    {
        uint32_t buddy;

        cur_order--;
        buddy = (uint32_t)index + (1U << cur_order);
        free_list_push(cur_order, buddy);
    }

    page_map[(uint32_t)index].order = order;
    page_map[(uint32_t)index].flags = 0;
    free_pages_count -= (1U << order);
    return (uint32_t)index;
}

void page_alloc_init(void)
{
    uint64_t kernel_end_phys;
    uint64_t free_start;
    uint64_t free_end;
    uint64_t phys;
    uint32_t i;

    alloc_base_phys = PHYS_OFFSET;
    memset((int8_t *)page_map, 0, sizeof(page_map));
    for (i = 0; i <= PAGE_ALLOC_MAX_ORDER; i++)
    {
        free_area[i] = -1;
    }

    free_pages_count = 0;

    kernel_end_phys = kernel_virt_to_phys((uint64_t)&__kernel_end);
    free_start = PAGE_ALIGN_UP(kernel_end_phys);
    free_end = alloc_base_phys + TOTAL_MEMORY;

    if (free_start < alloc_base_phys)
    {
        free_start = alloc_base_phys;
    }
    if (free_end < free_start)
    {
        free_end = free_start;
    }

    phys = free_start;
    while (phys < free_end)
    {
        uint64_t remain;
        uint32_t order;
        uint64_t block;

        remain = free_end - phys;
        order = order_for_count((uint32_t)(remain / PAGE_SIZE));
        block = PAGE_SIZE << order;
        while (order > 0 && (((phys | block) & (block - 1)) != 0 || block > remain))
        {
            order--;
            block = PAGE_SIZE << order;
        }

        free_block(phys_to_page_index(phys), order);
        phys += block;
    }

    printk("[page_alloc\tinit]: buddy ready, free=%u pages\n", free_pages_count);
}

uint64_t alloc_pages_phys(uint32_t order)
{
    uint32_t index;

    if (order > PAGE_ALLOC_MAX_ORDER)
    {
        return 0;
    }

    index = alloc_block(order);
    if (index == (uint32_t)-1)
    {
        return 0;
    }

    return page_index_to_phys(index);
}

void *alloc_pages(uint32_t order)
{
    uint64_t phys;

    phys = alloc_pages_phys(order);
    if (!phys)
    {
        return 0;
    }

    return (void *)linear_phys_to_virt(phys);
}

void free_pages_phys(uint64_t phys, uint32_t order)
{
    uint32_t index;

    if (order > PAGE_ALLOC_MAX_ORDER)
    {
        return;
    }

    if (phys < alloc_base_phys || phys >= alloc_base_phys + TOTAL_MEMORY)
    {
        return;
    }

    index = phys_to_page_index(PAGE_ALIGN_DOWN(phys));
    page_map[index].order = order;
    free_block(index, order);
}

void free_pages(void *addr, uint32_t order)
{
    free_pages_phys(kernel_virt_to_phys((uint64_t)addr), order);
}

uint32_t page_alloc_free_pages(void)
{
    return free_pages_count;
}

uint32_t page_alloc_total_pages(void)
{
    return page_count();
}
