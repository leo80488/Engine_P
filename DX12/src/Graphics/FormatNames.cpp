// FormatNames.cpp - see the header.
#include "Graphics/FormatNames.h"

namespace RHI
{
    const char* FormatName(RHI::Format f)
    {
        using F = RHI::Format;
        switch (f)
        {
        case F::R32G32B32A32_FLOAT:   return "R32G32B32A32_FLOAT";
        case F::R32G32B32A32_UINT:    return "R32G32B32A32_UINT";
        case F::R32G32B32A32_SINT:    return "R32G32B32A32_SINT";
        case F::R32G32B32_FLOAT:      return "R32G32B32_FLOAT";
        case F::R32G32B32_UINT:       return "R32G32B32_UINT";
        case F::R32G32B32_SINT:       return "R32G32B32_SINT";
        case F::R16G16B16A16_FLOAT:   return "R16G16B16A16_FLOAT";
        case F::R16G16B16A16_UNORM:   return "R16G16B16A16_UNORM";
        case F::R16G16B16A16_UINT:    return "R16G16B16A16_UINT";
        case F::R16G16B16A16_SNORM:   return "R16G16B16A16_SNORM";
        case F::R16G16B16A16_SINT:    return "R16G16B16A16_SINT";
        case F::R32G32_FLOAT:         return "R32G32_FLOAT";
        case F::R32G32_UINT:          return "R32G32_UINT";
        case F::R32G32_SINT:          return "R32G32_SINT";
        case F::D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
        case F::R10G10B10A2_UNORM:    return "R10G10B10A2_UNORM";
        case F::R10G10B10A2_UINT:     return "R10G10B10A2_UINT";
        case F::R11G11B10_FLOAT:      return "R11G11B10_FLOAT";
        case F::R8G8B8A8_UNORM:       return "R8G8B8A8_UNORM";
        case F::R8G8B8A8_UNORM_SRGB:  return "R8G8B8A8_UNORM_SRGB";
        case F::R8G8B8A8_UINT:        return "R8G8B8A8_UINT";
        case F::R8G8B8A8_SNORM:       return "R8G8B8A8_SNORM";
        case F::R8G8B8A8_SINT:        return "R8G8B8A8_SINT";
        case F::B8G8R8A8_UNORM:       return "B8G8R8A8_UNORM";
        case F::B8G8R8A8_UNORM_SRGB:  return "B8G8R8A8_UNORM_SRGB";
        case F::R16G16_FLOAT:         return "R16G16_FLOAT";
        case F::R16G16_UNORM:         return "R16G16_UNORM";
        case F::R16G16_UINT:          return "R16G16_UINT";
        case F::R16G16_SNORM:         return "R16G16_SNORM";
        case F::R16G16_SINT:          return "R16G16_SINT";
        case F::D32_FLOAT:            return "D32_FLOAT";
        case F::R32_FLOAT:            return "R32_FLOAT";
        case F::R32_UINT:             return "R32_UINT";
        case F::R32_SINT:             return "R32_SINT";
        case F::D24_UNORM_S8_UINT:    return "D24_UNORM_S8_UINT";
        case F::R9G9B9E5_SHAREDEXP:   return "R9G9B9E5_SHAREDEXP";
        case F::R8G8_UNORM:           return "R8G8_UNORM";
        case F::R8G8_UINT:            return "R8G8_UINT";
        case F::R8G8_SNORM:           return "R8G8_SNORM";
        case F::R8G8_SINT:            return "R8G8_SINT";
        case F::R16_FLOAT:            return "R16_FLOAT";
        case F::D16_UNORM:            return "D16_UNORM";
        case F::R16_UNORM:            return "R16_UNORM";
        case F::R16_UINT:             return "R16_UINT";
        case F::R16_SNORM:            return "R16_SNORM";
        case F::R16_SINT:             return "R16_SINT";
        case F::R8_UNORM:             return "R8_UNORM";
        case F::R8_UINT:              return "R8_UINT";
        case F::R8_SNORM:             return "R8_SNORM";
        case F::R8_SINT:              return "R8_SINT";
        case F::BC1_UNORM:            return "BC1_UNORM";
        case F::BC1_UNORM_SRGB:       return "BC1_UNORM_SRGB";
        case F::BC2_UNORM:            return "BC2_UNORM";
        case F::BC2_UNORM_SRGB:       return "BC2_UNORM_SRGB";
        case F::BC3_UNORM:            return "BC3_UNORM";
        case F::BC3_UNORM_SRGB:       return "BC3_UNORM_SRGB";
        case F::BC4_UNORM:            return "BC4_UNORM";
        case F::BC4_SNORM:            return "BC4_SNORM";
        case F::BC5_UNORM:            return "BC5_UNORM";
        case F::BC5_SNORM:            return "BC5_SNORM";
        case F::BC6H_UF16:            return "BC6H_UF16";
        case F::BC6H_SF16:            return "BC6H_SF16";
        case F::BC7_UNORM:            return "BC7_UNORM";
        case F::BC7_UNORM_SRGB:       return "BC7_UNORM_SRGB";
        case F::NV12:                 return "NV12";
        default:                      return "UNKNOWN";
        }
    }

    const char* DxgiFormatName(uint32_t dxgiFormat)
    {
        // Generated from the Windows SDK's dxgiformat.h (10.0.22621.0): the
        // values are the enum's, the names its enumerators minus the prefix.
        switch (dxgiFormat)
        {
        case 0: return "UNKNOWN";
        case 1: return "R32G32B32A32_TYPELESS";
        case 2: return "R32G32B32A32_FLOAT";
        case 3: return "R32G32B32A32_UINT";
        case 4: return "R32G32B32A32_SINT";
        case 5: return "R32G32B32_TYPELESS";
        case 6: return "R32G32B32_FLOAT";
        case 7: return "R32G32B32_UINT";
        case 8: return "R32G32B32_SINT";
        case 9: return "R16G16B16A16_TYPELESS";
        case 10: return "R16G16B16A16_FLOAT";
        case 11: return "R16G16B16A16_UNORM";
        case 12: return "R16G16B16A16_UINT";
        case 13: return "R16G16B16A16_SNORM";
        case 14: return "R16G16B16A16_SINT";
        case 15: return "R32G32_TYPELESS";
        case 16: return "R32G32_FLOAT";
        case 17: return "R32G32_UINT";
        case 18: return "R32G32_SINT";
        case 19: return "R32G8X24_TYPELESS";
        case 20: return "D32_FLOAT_S8X24_UINT";
        case 21: return "R32_FLOAT_X8X24_TYPELESS";
        case 22: return "X32_TYPELESS_G8X24_UINT";
        case 23: return "R10G10B10A2_TYPELESS";
        case 24: return "R10G10B10A2_UNORM";
        case 25: return "R10G10B10A2_UINT";
        case 26: return "R11G11B10_FLOAT";
        case 27: return "R8G8B8A8_TYPELESS";
        case 28: return "R8G8B8A8_UNORM";
        case 29: return "R8G8B8A8_UNORM_SRGB";
        case 30: return "R8G8B8A8_UINT";
        case 31: return "R8G8B8A8_SNORM";
        case 32: return "R8G8B8A8_SINT";
        case 33: return "R16G16_TYPELESS";
        case 34: return "R16G16_FLOAT";
        case 35: return "R16G16_UNORM";
        case 36: return "R16G16_UINT";
        case 37: return "R16G16_SNORM";
        case 38: return "R16G16_SINT";
        case 39: return "R32_TYPELESS";
        case 40: return "D32_FLOAT";
        case 41: return "R32_FLOAT";
        case 42: return "R32_UINT";
        case 43: return "R32_SINT";
        case 44: return "R24G8_TYPELESS";
        case 45: return "D24_UNORM_S8_UINT";
        case 46: return "R24_UNORM_X8_TYPELESS";
        case 47: return "X24_TYPELESS_G8_UINT";
        case 48: return "R8G8_TYPELESS";
        case 49: return "R8G8_UNORM";
        case 50: return "R8G8_UINT";
        case 51: return "R8G8_SNORM";
        case 52: return "R8G8_SINT";
        case 53: return "R16_TYPELESS";
        case 54: return "R16_FLOAT";
        case 55: return "D16_UNORM";
        case 56: return "R16_UNORM";
        case 57: return "R16_UINT";
        case 58: return "R16_SNORM";
        case 59: return "R16_SINT";
        case 60: return "R8_TYPELESS";
        case 61: return "R8_UNORM";
        case 62: return "R8_UINT";
        case 63: return "R8_SNORM";
        case 64: return "R8_SINT";
        case 65: return "A8_UNORM";
        case 66: return "R1_UNORM";
        case 67: return "R9G9B9E5_SHAREDEXP";
        case 68: return "R8G8_B8G8_UNORM";
        case 69: return "G8R8_G8B8_UNORM";
        case 70: return "BC1_TYPELESS";
        case 71: return "BC1_UNORM";
        case 72: return "BC1_UNORM_SRGB";
        case 73: return "BC2_TYPELESS";
        case 74: return "BC2_UNORM";
        case 75: return "BC2_UNORM_SRGB";
        case 76: return "BC3_TYPELESS";
        case 77: return "BC3_UNORM";
        case 78: return "BC3_UNORM_SRGB";
        case 79: return "BC4_TYPELESS";
        case 80: return "BC4_UNORM";
        case 81: return "BC4_SNORM";
        case 82: return "BC5_TYPELESS";
        case 83: return "BC5_UNORM";
        case 84: return "BC5_SNORM";
        case 85: return "B5G6R5_UNORM";
        case 86: return "B5G5R5A1_UNORM";
        case 87: return "B8G8R8A8_UNORM";
        case 88: return "B8G8R8X8_UNORM";
        case 89: return "R10G10B10_XR_BIAS_A2_UNORM";
        case 90: return "B8G8R8A8_TYPELESS";
        case 91: return "B8G8R8A8_UNORM_SRGB";
        case 92: return "B8G8R8X8_TYPELESS";
        case 93: return "B8G8R8X8_UNORM_SRGB";
        case 94: return "BC6H_TYPELESS";
        case 95: return "BC6H_UF16";
        case 96: return "BC6H_SF16";
        case 97: return "BC7_TYPELESS";
        case 98: return "BC7_UNORM";
        case 99: return "BC7_UNORM_SRGB";
        case 100: return "AYUV";
        case 101: return "Y410";
        case 102: return "Y416";
        case 103: return "NV12";
        case 104: return "P010";
        case 105: return "P016";
        case 106: return "420_OPAQUE";
        case 107: return "YUY2";
        case 108: return "Y210";
        case 109: return "Y216";
        case 110: return "NV11";
        case 111: return "AI44";
        case 112: return "IA44";
        case 113: return "P8";
        case 114: return "A8P8";
        case 115: return "B4G4R4A4_UNORM";
        case 130: return "P208";
        case 131: return "V208";
        case 132: return "V408";
        case 189: return "SAMPLER_FEEDBACK_MIN_MIP_OPAQUE";
        case 190: return "SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE";
        default: return nullptr;
        }
    }

    std::string DxgiFormatLabel(uint32_t dxgiFormat)
    {
        switch (dxgiFormat)
        {
        case 71: return "BC1";        case 72: return "BC1 sRGB";
        case 74: return "BC2";        case 75: return "BC2 sRGB";
        case 77: return "BC3";        case 78: return "BC3 sRGB";
        case 80: return "BC4";        case 81: return "BC4 SNORM";
        case 83: return "BC5";        case 84: return "BC5 SNORM";
        case 95: return "BC6H UF16";  case 96: return "BC6H SF16";
        case 98: return "BC7";        case 99: return "BC7 sRGB";
        case 2:  return "RGBA32F";    case 6:  return "RGB32F";
        case 10: return "RGBA16F";    case 11: return "RGBA16";
        case 13: return "RGBA16 SNORM";
        case 16: return "RG32F";      case 24: return "RGB10A2";
        case 26: return "R11G11B10F";
        case 28: return "RGBA8";      case 29: return "RGBA8 sRGB";
        case 31: return "RGBA8 SNORM";
        case 34: return "RG16F";      case 35: return "RG16";
        case 41: return "R32F";       case 49: return "RG8";
        case 54: return "R16F";       case 56: return "R16";
        case 61: return "R8";         case 65: return "A8";
        case 67: return "RGB9E5";
        case 87: return "BGRA8";      case 88: return "BGRX8";
        case 91: return "BGRA8 sRGB"; case 93: return "BGRX8 sRGB";
        default: break;
        }
        if (const char* n = DxgiFormatName(dxgiFormat)) return n;
        return "DXGI " + std::to_string(dxgiFormat);
    }
}
