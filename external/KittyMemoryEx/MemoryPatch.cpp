#include "MemoryPatch.hpp"

MemoryPatch::MemoryPatch()
{
    _pMem = nullptr;
    _address = 0;
    _size = 0;
    _orig_code.clear();
    _patch_code.clear();
}

MemoryPatch::~MemoryPatch()
{
    // clean up
    _orig_code.clear();
    _orig_code.shrink_to_fit();

    _patch_code.clear();
    _patch_code.shrink_to_fit();
}

MemoryPatch::MemoryPatch(IKittyMemOp *pMem, uintptr_t absolute_address, const void *patch_code, size_t patch_size)
{
    _pMem = nullptr;
    _address = 0;
    _size = 0;
    _orig_code.clear();
    _patch_code.clear();

    if (!pMem || !absolute_address || !patch_code || !patch_size)
        return;

    _pMem = pMem;
    _address = absolute_address;
    _size = patch_size;
    _orig_code.resize(patch_size);
    _patch_code.resize(patch_size);

    // initialize patch
    memcpy(&_patch_code[0], patch_code, patch_size);

    // backup current content
    _pMem->Read(_address, &_orig_code[0], _size);
}

bool MemoryPatch::isValid() const
{
    return (_pMem && _address && _size && _orig_code.size() == _size && _patch_code.size() == _size);
}

size_t MemoryPatch::get_PatchSize() const
{
    return _size;
}

uintptr_t MemoryPatch::get_TargetAddress() const
{
    return _address;
}

bool MemoryPatch::Restore()
{
    if (!isValid())
        return false;

    return _pMem->Write(_address, &_orig_code[0], _size);
}

bool MemoryPatch::Modify()
{
    if (!isValid())
        return false;

    return _pMem->Write(_address, &_patch_code[0], _size);
}

std::string MemoryPatch::get_CurrBytes() const
{
    if (!isValid())
        return "";

    std::vector<uint8_t> buffer(_size);
    _pMem->Read(_address, &buffer[0], _size);

    return KittyUtils::Data::toHex(&buffer[0], _size);
}

std::string MemoryPatch::get_OrigBytes() const
{
    if (!isValid())
        return "";

    return KittyUtils::Data::toHex(&_orig_code[0], _orig_code.size());
}

std::string MemoryPatch::get_PatchBytes() const
{
    if (!isValid())
        return "";

    return KittyUtils::Data::toHex(&_patch_code[0], _patch_code.size());
}

/* ============================== MemoryPatchMgr ============================== */

MemoryPatch MemoryPatchMgr::createWithBytes(uintptr_t absolute_address, const void *patch_code, size_t patch_size)
{
    return MemoryPatch(_pMem, absolute_address, patch_code, patch_size);
}

MemoryPatch MemoryPatchMgr::createWithHex(uintptr_t absolute_address, std::string hex)
{
    if (!absolute_address || !KittyUtils::String::validateHex(hex))
        return MemoryPatch();

    std::vector<uint8_t> patch_code(hex.length() / 2);
    KittyUtils::Data::fromHex(hex, &patch_code[0]);

    return MemoryPatch(_pMem, absolute_address, patch_code.data(), patch_code.size());
}
