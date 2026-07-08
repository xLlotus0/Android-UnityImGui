#pragma once

#include "KittyUtils.hpp"
#include "KittyMemoryEx.hpp"
#include "KittyMemOp.hpp"

/**
 * @brief The MemoryPatch class represents a memory patch for applying modifications to a memory address.
 *
 * @details The MemoryPatch class allows for the creation and management of memory patches, including:
 * - Applying patches with byte data
 * - Applying patches with hexadecimal strings
 * - Checking the validity of a MemoryPatch object
 * - Restoring the patch to its original value
 * - Modifying the memory target to apply the patch
 * - Retrieving the hex strings of current, original, and patch bytes
 */
class MemoryPatch
{
    friend class MemoryPatchMgr;

private:
    IKittyMemOp *_pMem;

    uintptr_t _address;
    size_t _size;

    std::vector<uint8_t> _orig_code;
    std::vector<uint8_t> _patch_code;

public:
    MemoryPatch();
    ~MemoryPatch();

    MemoryPatch(IKittyMemOp *pMem, uintptr_t absolute_address, const void *patch_code, size_t patch_size);

    /**
     * @brief Checks if the MemoryPatch object is valid.
     */
    bool isValid() const;

    /**
     * @brief Returns the size of the memory patch.
     */
    size_t get_PatchSize() const;

    /**
     * @brief Returns the target address of the memory patch.
     */
    uintptr_t get_TargetAddress() const;

    /**
     * @brief Restores the patch to its original value
     */
    bool Restore();

    /**
     * @brief Applies patch modifications to the target address
     */
    bool Modify();

    /**
     * @brief Returns hex string of the current target address bytes
     */
    std::string get_CurrBytes() const;

    /**
     * @brief Returns hex string of the original bytes
     */
    std::string get_OrigBytes() const;

    /**
     * @brief Returns hex string of the patch bytes
     */
    std::string get_PatchBytes() const;
};

/**
 * @brief The MemoryPatch manager class.
 */
class MemoryPatchMgr
{
private:
    IKittyMemOp *_pMem;

public:
    MemoryPatchMgr() : _pMem(nullptr)
    {
    }

    /**
     * @brief Constructor for MemoryPatchMgr.
     * @param pMem A pointer to the memory operation object.
     */
    MemoryPatchMgr(IKittyMemOp *pMem) : _pMem(pMem)
    {
    }

    /**
     * @brief Creates a memory patch with byte data.
     *
     * @param absolute_address The absolute address in memory where the patch will be applied.
     * @param patch_code A pointer to the patch code to be applied.
     * @param patch_size The size of the patch code in bytes.
     */
    MemoryPatch createWithBytes(uintptr_t absolute_address, const void *patch_code, size_t patch_size);

    /**
     * @brief Creates a memory patch with hexadecimal string data.
     *
     * @param absolute_address The absolute address in memory where the patch will be applied.
     * @param hex A hexadecimal string representing the patch code.
     */
    MemoryPatch createWithHex(uintptr_t absolute_address, std::string hex);
};
