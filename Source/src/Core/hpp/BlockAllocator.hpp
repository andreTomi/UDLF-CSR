#ifndef BLOCK_ALLOCATOR_HPP
#define BLOCK_ALLOCATOR_HPP

#include <vector>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <new>

// Platform specific includes for Huge Pages
#if defined(__linux__)
    #include <sys/mman.h>
    #include <unistd.h>
#endif

/**
 * @brief High-Performance Linear Block Allocator.
 * * Optimized for "Emit-Sort-Reduce" patterns where millions of small objects
 * (like Triplets) are generated in a stream and then freed all at once.
 * * Features:
 * - Huge Page Support (2MB alignment) to reduce TLB misses.
 * - Lock-Free (Designed for thread-local usage).
 * - Iterable (Can act as a data container).
 */
template <typename T>
class alignas(64) BlockAllocator {
public:
    // 2MB Block Size aligns with Transparent Huge Pages (THP) on Linux
    static constexpr size_t BLOCK_SIZE_BYTES = 2 * 1024 * 1024; 
    static constexpr size_t ITEMS_PER_BLOCK = BLOCK_SIZE_BYTES / sizeof(T);

    BlockAllocator() : current_block_(nullptr), current_index_(0) {
        allocate_new_block();
    }

    ~BlockAllocator() {
        for (void* ptr : blocks_) {
            // Free raw memory. Destructors are NOT called (T assumed POD).
            // Use std::free because posix_memalign/aligned_alloc was used.
            std::free(ptr);
        }
    }

    // Disable copy/move to prevent double-free issues
    BlockAllocator(const BlockAllocator&) = delete;
    BlockAllocator& operator=(const BlockAllocator&) = delete;
    BlockAllocator(BlockAllocator&&) = delete;
    BlockAllocator& operator=(BlockAllocator&&) = delete;

    /**
     * @brief Allocates space for one item.
     * @return Pointer to uninitialized memory.
     */
    [[nodiscard]] __attribute__((always_inline)) 
    T* allocate() {
        if (__builtin_expect(current_index_ >= ITEMS_PER_BLOCK, 0)) {
            allocate_new_block();
        }
        return &current_block_[current_index_++];
    }

    /**
     * @brief Allocates N items contiguously.
     * Only works if N fits in the remainder of the block, otherwise forces new block.
     * Note: Does not support N > ITEMS_PER_BLOCK (2MB).
     */
    T* allocate_n(size_t n) {
        if (n > ITEMS_PER_BLOCK) throw std::bad_alloc(); 

        if (current_index_ + n > ITEMS_PER_BLOCK) {
            allocate_new_block();
        }
        
        T* ptr = &current_block_[current_index_];
        current_index_ += n;
        return ptr;
    }

    // =========================================================================
    // Container-Like Interface (For copying/iterating)
    // =========================================================================

    size_t size() const {
        if (blocks_.empty()) return 0;
        return (blocks_.size() - 1) * ITEMS_PER_BLOCK + current_index_;
    }

#ifdef PHASE_PROFILE
    // Phase-profile only (A0/A3 memory ledger): bytes reserved by this
    // allocator's blocks and how many blocks were taken. Inert -- no layout,
    // no branch, no allocation change -- when the flag is absent.
    size_t capacity_bytes() const { return blocks_.size() * BLOCK_SIZE_BYTES; }
    size_t blocks_count() const { return blocks_.size(); }
#endif

    /**
     * @brief Copies all allocated items into a destination iterator.
     * Used to merge thread-local buffers into the main CSR array.
     */
    template <typename OutputIt>
    void copy_to(OutputIt dest) const {
        // Copy full blocks
        for (size_t i = 0; i < blocks_.size() - 1; ++i) {
            T* block = blocks_[i];
            std::copy(block, block + ITEMS_PER_BLOCK, dest);
            std::advance(dest, ITEMS_PER_BLOCK);
        }
        
        // Copy partial active block
        if (current_block_) {
            std::copy(current_block_, current_block_ + current_index_, dest);
        }
    }
    
    /**
     * @brief Iterates over all items.
     */
    template <typename Func>
    void for_each(Func f) const {
        for (size_t i = 0; i < blocks_.size() - 1; ++i) {
            T* block = blocks_[i];
            for (size_t j = 0; j < ITEMS_PER_BLOCK; ++j) {
                f(block[j]);
            }
        }
        if (current_block_) {
            for (size_t j = 0; j < current_index_; ++j) {
                f(current_block_[j]);
            }
        }
    }

private:
    void allocate_new_block() {
        void* ptr = nullptr;

        // Try to allocate 2MB aligned memory
#if defined(__linux__) && defined(_GNU_SOURCE)
        // posix_memalign is standard on Linux/Unix
        if (posix_memalign(&ptr, 2 * 1024 * 1024, BLOCK_SIZE_BYTES) != 0) {
            // Fallback to standard malloc if alignment fails
            ptr = std::malloc(BLOCK_SIZE_BYTES);
        }
        
        // Hint the kernel to use Huge Pages (reduces TLB misses significantly)
        if (ptr) {
            madvise(ptr, BLOCK_SIZE_BYTES, MADV_SEQUENTIAL | MADV_HUGEPAGE);
        }
#else
        // Generic fallback for non-Linux
        ptr = std::malloc(BLOCK_SIZE_BYTES);
#endif

        if (!ptr) throw std::bad_alloc();

        blocks_.push_back(static_cast<T*>(ptr));
        current_block_ = static_cast<T*>(ptr);
        current_index_ = 0;
    }

    std::vector<T*> blocks_;
    T* current_block_;
    size_t current_index_;
};

#endif // BLOCK_ALLOCATOR_HPP