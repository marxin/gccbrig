/* brig-util.cc -- gccbrig utility functions
   Copyright (C) 2016 Free Software Foundation, Inc.
   Contributed by Pekka Jaaskelainen <pekka.jaaskelainen@parmance.com>
   for General Processor Tech.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

// Some code reused from Martin Jambor's gcc-hsa tree.

/* Return true if operand number OPNUM of instruction with OPCODE is an output.
   False if it is an input.  */

#include <sstream>

#include "stdint.h"
#include "hsa-brig-format.h"
#include "brig-util.h"
#include "errors.h"

bool
gccbrig_hsa_opcode_op_output_p (BrigOpcode16_t opcode, int opnum)
{
  switch (opcode)
    {
    case BRIG_OPCODE_BR:
    case BRIG_OPCODE_SBR:
    case BRIG_OPCODE_CBR:
    case BRIG_OPCODE_ST:
    case BRIG_OPCODE_ATOMICNORET:
    case BRIG_OPCODE_SIGNALNORET:
    case BRIG_OPCODE_INITFBAR:
    case BRIG_OPCODE_JOINFBAR:
    case BRIG_OPCODE_WAITFBAR:
    case BRIG_OPCODE_ARRIVEFBAR:
    case BRIG_OPCODE_LEAVEFBAR:
    case BRIG_OPCODE_RELEASEFBAR:
    case BRIG_OPCODE_DEBUGTRAP:
      return false;
    default:
      return opnum == 0;
    }
}

unsigned
gccbrig_hsa_type_bit_size (BrigType16_t t)
{

  unsigned pack_type = t & ~0x01F;

  if (pack_type == BRIG_TYPE_PACK_32)
    return 32;
  else if (pack_type == BRIG_TYPE_PACK_64)
    return 64;
  else if (pack_type == BRIG_TYPE_PACK_128)
    return 128;

  switch (t)
    {
    case BRIG_TYPE_B1:
      return 1;

    case BRIG_TYPE_U8:
    case BRIG_TYPE_S8:
    case BRIG_TYPE_B8:
      return 8;

    case BRIG_TYPE_U16:
    case BRIG_TYPE_S16:
    case BRIG_TYPE_B16:
    case BRIG_TYPE_F16:
      return 16;

    case BRIG_TYPE_U32:
    case BRIG_TYPE_S32:
    case BRIG_TYPE_B32:
    case BRIG_TYPE_F32:
    case BRIG_TYPE_U8X4:
    case BRIG_TYPE_U16X2:
    case BRIG_TYPE_S8X4:
    case BRIG_TYPE_S16X2:
    case BRIG_TYPE_F16X2:
    case BRIG_TYPE_SIG32:
      return 32;

    case BRIG_TYPE_U64:
    case BRIG_TYPE_S64:
    case BRIG_TYPE_F64:
    case BRIG_TYPE_B64:
    case BRIG_TYPE_U8X8:
    case BRIG_TYPE_U16X4:
    case BRIG_TYPE_U32X2:
    case BRIG_TYPE_S8X8:
    case BRIG_TYPE_S16X4:
    case BRIG_TYPE_S32X2:
    case BRIG_TYPE_F16X4:
    case BRIG_TYPE_F32X2:
    case BRIG_TYPE_SIG64:
      return 64;

    case BRIG_TYPE_B128:
    case BRIG_TYPE_U8X16:
    case BRIG_TYPE_U16X8:
    case BRIG_TYPE_U32X4:
    case BRIG_TYPE_U64X2:
    case BRIG_TYPE_S8X16:
    case BRIG_TYPE_S16X8:
    case BRIG_TYPE_S32X4:
    case BRIG_TYPE_S64X2:
    case BRIG_TYPE_F16X8:
    case BRIG_TYPE_F32X4:
    case BRIG_TYPE_F64X2:
      return 128;

    default:
      // Uknown size.
      return 0;
    }
}

// gcc-hsa borrowed code ENDS

uint64_t
gccbrig_to_uint64_t (const BrigUInt64 &brig_type)
{
  return (uint64_t (brig_type.hi) << 32) | uint64_t (brig_type.lo);
}

int
gccbrig_reg_size (const BrigOperandRegister *brig_reg)
{
  switch (brig_reg->regKind)
    {
    case BRIG_REGISTER_KIND_CONTROL:
      return 1;
    case BRIG_REGISTER_KIND_SINGLE:
      return 32;
    case BRIG_REGISTER_KIND_DOUBLE:
      return 64;
    case BRIG_REGISTER_KIND_QUAD:
      return 128;
    default:
      internal_error ("reg operand kind %u", brig_reg->regKind);
      break;
    }
}

std::string
gccbrig_reg_name (const BrigOperandRegister *reg)
{
  std::ostringstream strstr;
  switch (reg->regKind)
    {
    case BRIG_REGISTER_KIND_CONTROL:
      strstr << 'c';
      break;
    case BRIG_REGISTER_KIND_SINGLE:
      strstr << 's';
      break;
    case BRIG_REGISTER_KIND_DOUBLE:
      strstr << 'd';
      break;
    case BRIG_REGISTER_KIND_QUAD:
      strstr << 'q';
      break;
    default:
      gcc_unreachable ();
      return "";
    }
  strstr << reg->regNum;
  return strstr.str ();
}

std::string
gccbrig_type_name (BrigType16_t type)
{
  switch (type)
    {
    case BRIG_TYPE_U8:
      return "u8";
    case BRIG_TYPE_U16:
      return "u16";
    case BRIG_TYPE_U32:
      return "u32";
    case BRIG_TYPE_U64:
      return "u64";
    case BRIG_TYPE_S8:
      return "s8";
    case BRIG_TYPE_S16:
      return "s16";
    case BRIG_TYPE_S32:
      return "s32";
    case BRIG_TYPE_S64:
      return "s64";
    default:
      internal_error ("Unsupported type %d.", type);
      break;
    }
}

std::string
gccbrig_segment_name (BrigSegment8_t segment)
{
  if (segment == BRIG_SEGMENT_GLOBAL)
    return "global";
  else if (segment == BRIG_SEGMENT_GROUP)
    return "group";
  else if (segment == BRIG_SEGMENT_PRIVATE)
    return "private";
  else
    internal_error ("unknown segment %u", segment);
}

bool
gccbrig_is_float_type (BrigType16_t type)
{
  return (type == BRIG_TYPE_F32 || type == BRIG_TYPE_F64
	  || type == BRIG_TYPE_F16);
}

BrigType16_t
gccbrig_tree_type_to_hsa_type (tree tree_type)
{
  if (INTEGRAL_TYPE_P (tree_type))
    {
      if (TYPE_UNSIGNED (tree_type))
	{
	  switch (int_size_in_bytes (tree_type))
	    {
	    case 1:
	      return BRIG_TYPE_U8;
	    case 2:
	      return BRIG_TYPE_U16;
	    case 4:
	      return BRIG_TYPE_U32;
	    case 8:
	      return BRIG_TYPE_U64;
	    default:
	      break;
	    }
	}
      else
	{
	  switch (int_size_in_bytes (tree_type))
	    {
	    case 1:
	      return BRIG_TYPE_S8;
	    case 2:
	      return BRIG_TYPE_S16;
	    case 4:
	      return BRIG_TYPE_S32;
	    case 8:
	      return BRIG_TYPE_S64;
	    default:
	      break;
	    }
	}
    }
  else if (VECTOR_TYPE_P (tree_type))
    {
      tree element_type = TREE_TYPE (tree_type);
      size_t element_size = int_size_in_bytes (element_type) * 8;
      BrigType16_t brig_element_type;
      switch (element_size)
	{
	case 8:
	  brig_element_type
	    = TYPE_UNSIGNED (element_type) ? BRIG_TYPE_U8 : BRIG_TYPE_S8;
	  break;
	case 16:
	  brig_element_type
	    = TYPE_UNSIGNED (element_type) ? BRIG_TYPE_U16 : BRIG_TYPE_S16;
	  break;
	case 32:
	  brig_element_type
	    = TYPE_UNSIGNED (element_type) ? BRIG_TYPE_U32 : BRIG_TYPE_S32;
	  break;
	case 64:
	  brig_element_type
	    = TYPE_UNSIGNED (element_type) ? BRIG_TYPE_U64 : BRIG_TYPE_S64;
	  break;
	default:
	  internal_error ("Cannot convert the given type to HSA type.");
	}

      BrigType16_t pack_type;
      switch (int_size_in_bytes (tree_type) * 8)
	{
	case 32:
	  pack_type = BRIG_TYPE_PACK_32;
	  break;
	case 64:
	  pack_type = BRIG_TYPE_PACK_64;
	  break;
	case 128:
	  pack_type = BRIG_TYPE_PACK_128;
	  break;
	default:
	  internal_error ("Cannot convert the given type to HSA type.");
	}
      return brig_element_type | pack_type;
    }
  internal_error ("Cannot convert the given type to HSA type.");
}

// Returns true in case the operation is a "bit level" operation,
// not having operand type depending differences in the semantics.
bool
gccbrig_is_raw_operation (BrigOpcode16_t opcode)
{
  return opcode == BRIG_OPCODE_CMOV || opcode == BRIG_OPCODE_SHUFFLE
	 || opcode == BRIG_OPCODE_UNPACK || opcode == BRIG_OPCODE_UNPACKLO
	 || opcode == BRIG_OPCODE_UNPACKHI || opcode == BRIG_OPCODE_ST
	 || opcode == BRIG_OPCODE_PACK;
}
