/* brig-basic-inst-handler.cc -- brig basic instruction handling
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

#include <sstream>

#include "brig-code-entry-handler.h"
#include "brig-util.h"

#include "errors.h"
#include "gimple-expr.h"
#include "convert.h"
#include "print-tree.h"
#include "tree-pretty-print.h"
#include "langhooks.h"
#include "stor-layout.h"
#include "diagnostic-core.h"

brig_basic_inst_handler::brig_basic_inst_handler (brig_to_generic &parent)
  : brig_code_entry_handler (parent)
{
}

class scalarized_sat_arithmetics : public tree_element_binary_visitor
{
public:
  scalarized_sat_arithmetics (const BrigInstBase &brig_inst)
    : brig_inst_ (brig_inst)
  {
    std::ostringstream builtin_name;
    builtin_name << "__phsa_builtin_sat_";

    switch (brig_inst.opcode)
      {
      case BRIG_OPCODE_ADD:
	builtin_name << "add";
	break;
      case BRIG_OPCODE_SUB:
	builtin_name << "sub";
	break;
      case BRIG_OPCODE_MUL:
	builtin_name << "mul";
	break;
      default:
	internal_error ("Unsupported saturating opcode %d.", brig_inst.opcode);
      }
    BrigType16_t element_type = brig_inst.type & 0x01F;
    builtin_name << "_" << gccbrig_type_name (element_type);
    builtin_name_ = builtin_name.str ();
  }

  virtual tree
  visit_element (brig_code_entry_handler &, tree operand0, tree operand1)
  {
    /* Implement saturating arithmetics with scalar built-ins for now.
       TO OPTIMIZE: emit GENERIC nodes for the simplest cases or at least
       emit vector built-ins.	*/
    tree builtin = NULL_TREE;
    return call_builtin (&builtin, builtin_name_.c_str (), 2,
			 TREE_TYPE (operand0), TREE_TYPE (operand0), operand0,
			 TREE_TYPE (operand1), operand1);
  }
  const BrigInstBase &brig_inst_;
  std::string builtin_name_;
};

bool
brig_basic_inst_handler::must_be_scalarized (const BrigInstBase *brig_inst,
					     tree instr_type) const
{
  if (!VECTOR_TYPE_P (instr_type))
    return false;
  if (brig_inst->opcode != BRIG_OPCODE_MULHI)
    return false;

  // There is limited support for vector highpart mul nodes,
  // and it probably depends on the target which ones are
  // supported. TO DO: figure out a more robust way to ask this
  // from the target. can_mult_highpart_p () from optabs.c seems
  // not to be reliable enough.

  size_t elements = TYPE_VECTOR_SUBPARTS (instr_type);
  BrigType16_t element_type = brig_inst->type & 0x01F;
  if (elements < 16
      && (element_type == BRIG_TYPE_S8 || element_type == BRIG_TYPE_U8
	  || element_type == BRIG_TYPE_S16 || element_type == BRIG_TYPE_U16))
    return true;
  else
    return false;
}

// Returns a "raw" type (unsigned integer) with the same bit width as
// the given type.
tree
brig_basic_inst_handler::get_raw_type (tree orig_type)
{
  size_t esize = int_size_in_bytes (orig_type) * 8;
  return build_nonstandard_integer_type (esize, true);
}

tree
brig_basic_inst_handler::build_shuffle (tree arith_type, tree_stl_vec &operands)
{
  tree element_type = get_raw_type (TREE_TYPE (TREE_TYPE (operands[0])));

  // Offsets to add to the mask values to convert from the
  // HSAIL mask to VEC_PERM_EXPR masks. VEC_PERM_EXPR mask
  // assumes an index spanning from 0 to 2 times the vec
  // width while HSAIL refers separately to two different
  // input vectors, thus is not a "full shuffle" where all
  // output elements can originate from any input element.
  vec<constructor_elt, va_gc> *mask_offset_vals = NULL;

  vec<constructor_elt, va_gc> *input_mask_vals = NULL;
  size_t input_mask_element_size
    = exact_log2 (TYPE_VECTOR_SUBPARTS (arith_type));
  // Unpack the tightly packed mask elements to BIT_FIELD_REFs
  // from which to construct the mask vector as understood by
  // VEC_PERM_EXPR.
  tree mask_operand = add_temp_var ("shuffle_mask", operands[2]);

  tree mask_element_type
    = build_nonstandard_integer_type (input_mask_element_size, true);

  for (size_t i = 0; i < TYPE_VECTOR_SUBPARTS (arith_type); ++i)
    {
      tree mask_element = build3 (
	BIT_FIELD_REF, mask_element_type, mask_operand,
	build_int_cst (unsigned_char_type_node, input_mask_element_size),
	build_int_cst (unsigned_char_type_node, i * input_mask_element_size));

      mask_element = convert (element_type, mask_element);

      tree offset;
      if (i < TYPE_VECTOR_SUBPARTS (arith_type) / 2)
	offset = build_int_cst (element_type, 0);
      else
	offset
	  = build_int_cst (element_type, TYPE_VECTOR_SUBPARTS (arith_type));

      CONSTRUCTOR_APPEND_ELT (mask_offset_vals, NULL_TREE, offset);
      CONSTRUCTOR_APPEND_ELT (input_mask_vals, NULL_TREE, mask_element);
    }
  tree mask_vec_type
    = build_vector_type (element_type, TYPE_VECTOR_SUBPARTS (arith_type));

  tree mask_vec = build_constructor (mask_vec_type, input_mask_vals);
  tree offset_vec = build_constructor (mask_vec_type, mask_offset_vals);

  tree mask = build2 (PLUS_EXPR, mask_vec_type, mask_vec, offset_vec);

  tree perm = build3 (VEC_PERM_EXPR, TREE_TYPE (operands[0]), operands[0],
		      operands[1], mask);
  return perm;
}

tree
brig_basic_inst_handler::build_unpack (tree_stl_vec &operands)
{
  // Implement the unpack with a shuffle that stores the unpacked
  // element to the lowest bit positions in the dest. After that
  // a bit AND is used to clear the uppermost bits.
  tree src_element_type = TREE_TYPE (TREE_TYPE (operands[0]));

  // Perform the operations with a raw (unsigned int type) type.
  tree element_type = get_raw_type (src_element_type);

  vec<constructor_elt, va_gc> *input_mask_vals = NULL;
  vec<constructor_elt, va_gc> *and_mask_vals = NULL;

  size_t element_count = TYPE_VECTOR_SUBPARTS (TREE_TYPE (operands[0]));
  tree vec_type = build_vector_type (element_type, element_count);

  for (size_t i = 0; i < element_count; ++i)
    {
      tree mask_element;
      if (i == 0)
	mask_element = convert (element_type, operands[1]);
      else
	mask_element = build_int_cst (element_type, 0);

      CONSTRUCTOR_APPEND_ELT (input_mask_vals, NULL_TREE, mask_element);

      tree and_mask_element;
      if (i == 0)
	and_mask_element = build_int_cst (element_type, -1);
      else
	and_mask_element = build_int_cst (element_type, 0);
      CONSTRUCTOR_APPEND_ELT (and_mask_vals, NULL_TREE, and_mask_element);
    }

  tree mask_vec = build_constructor (vec_type, input_mask_vals);

  tree and_mask_vec = build_constructor (vec_type, and_mask_vals);

  tree perm = build3 (VEC_PERM_EXPR, vec_type,
		      build_reinterpret_cast (vec_type, operands[0]),
		      build_reinterpret_cast (vec_type, operands[0]), mask_vec);

  tree cleared = build2 (BIT_AND_EXPR, vec_type, perm, and_mask_vec);

  tree raw_type = build_nonstandard_integer_type (
    int_size_in_bytes (TREE_TYPE (cleared)) * 8, true);

  tree as_int = build_reinterpret_cast (raw_type, cleared);

  if (int_size_in_bytes (src_element_type) < 4)
    {
      if (INTEGRAL_TYPE_P (src_element_type))
	return extend_int (as_int, uint32_type_node, src_element_type);
    }
  return as_int;
}

tree
brig_basic_inst_handler::build_pack (tree_stl_vec &operands)
{
  // Implement pack using a bit level insertion.
  // TO OPTIMIZE: Reuse this for implementing the bitinsert
  // without a builtin call.

  size_t ecount = TYPE_VECTOR_SUBPARTS (TREE_TYPE (operands[0]));
  size_t vecsize = int_size_in_bytes (TREE_TYPE (operands[0])) * 8;
  tree wide_type = build_nonstandard_integer_type (vecsize, 1);

  tree src_vect = build_reinterpret_cast (wide_type, operands[0]);
  src_vect = add_temp_var ("src_vect", src_vect);

  tree scalar = operands[1];
  scalar = add_temp_var ("scalar", convert_to_integer (wide_type, scalar));

  tree pos = operands[2];

  // The upper bits of the position can contain garbage.
  // We need to zero them out for defined behavior.
  pos = add_temp_var (
    "pos",
    convert (wide_type, build2 (BIT_AND_EXPR, TREE_TYPE (pos), operands[2],
				build_int_cstu (TREE_TYPE (pos), ecount - 1))));

  tree element_type = TREE_TYPE (TREE_TYPE (operands[0]));
  size_t element_width = int_size_in_bytes (element_type) * 8;
  tree ewidth = build_int_cstu (wide_type, element_width);

  tree bitoffset = build2 (MULT_EXPR, wide_type, ewidth, pos);
  bitoffset = add_temp_var ("offset", bitoffset);

  uint64_t mask_int
    = element_width == 64 ? (uint64_t) -1 : ((uint64_t) 1 << element_width) - 1;

  tree mask = build_int_cstu (wide_type, mask_int);

  mask = add_temp_var ("mask", convert_to_integer (wide_type, mask));

  tree clearing_mask
    = build1 (BIT_NOT_EXPR, wide_type,
	      build2 (LSHIFT_EXPR, wide_type, mask, bitoffset));

  tree zeroed_element
    = build2 (BIT_AND_EXPR, wide_type, src_vect, clearing_mask);

  // TO OPTIMIZE: Is the AND necessary: does HSA define what
  // happens if the upper bits in the inserted element are not
  // zero?
  tree element_in_position
    = build2 (LSHIFT_EXPR, wide_type,
	      build2 (BIT_AND_EXPR, wide_type, scalar, mask), bitoffset);

  tree inserted
    = build2 (BIT_IOR_EXPR, wide_type, zeroed_element, element_in_position);
  return inserted;
}

tree
brig_basic_inst_handler::build_unpack_lo_or_hi (BrigOpcode16_t brig_opcode,
						tree arith_type,
						tree_stl_vec &operands)
{
  tree element_type = get_raw_type (TREE_TYPE (arith_type));
  tree mask_vec_type
    = build_vector_type (element_type, TYPE_VECTOR_SUBPARTS (arith_type));

  size_t element_count = TYPE_VECTOR_SUBPARTS (arith_type);
  vec<constructor_elt, va_gc> *input_mask_vals = NULL;

  size_t offset = (brig_opcode == BRIG_OPCODE_UNPACKLO) ? 0 : element_count / 2;

  for (size_t i = 0; i < element_count / 2; ++i)
    {
      CONSTRUCTOR_APPEND_ELT (input_mask_vals, NULL_TREE,
			      build_int_cst (element_type, offset + i));
      CONSTRUCTOR_APPEND_ELT (input_mask_vals, NULL_TREE,
			      build_int_cst (element_type,
					     offset + i + element_count));
    }

  tree mask_vec = build_constructor (mask_vec_type, input_mask_vals);

  tree perm = build3 (VEC_PERM_EXPR, TREE_TYPE (operands[0]), operands[0],
		      operands[1], mask_vec);
  return perm;
}

tree
brig_basic_inst_handler::build_instr_expr (BrigOpcode16_t brig_opcode,
					   BrigType16_t brig_type,
					   tree arith_type,
					   tree_stl_vec &operands)
{
  const int first_input
    = gccbrig_hsa_opcode_op_output_p (brig_opcode, 0) ? 1 : 0;

  tree_code opcode = get_tree_code_for_hsa_opcode (brig_opcode, brig_type);

  size_t input_count = operands.size ();
  size_t output_count = first_input;

  BrigType16_t inner_type = brig_type & 0x01f;

  tree instr_inner_type
    = VECTOR_TYPE_P (arith_type) ? TREE_TYPE (arith_type) : arith_type;

  if (opcode == RSHIFT_EXPR || opcode == LSHIFT_EXPR)
    {
      // HSA defines modulo/clipping behavior for shift amounts larger
      // than the bit width, while tree.def leaves it undefined.
      // We need to mask the upper bits to ensure the defined behavior.
      tree scalar_mask
	= build_int_cst (instr_inner_type,
			 gccbrig_hsa_type_bit_size (inner_type) - 1);

      tree mask = VECTOR_TYPE_P (arith_type)
		    ? build_vector_from_val (arith_type, scalar_mask)
		    : scalar_mask;

      // The shift amount is a scalar -- broadcast it to produce
      // a vector shift.
      if (VECTOR_TYPE_P (arith_type))
	operands[1] = build_vector_from_val (arith_type, operands[1]);
      operands[1] = build2 (BIT_AND_EXPR, arith_type, operands[1], mask);
    }

  if (opcode == TREE_LIST)
    {
      // There was no direct GENERIC opcode for the instruction;
      // try to emulate it with a chain of GENERIC nodes.
      if (brig_opcode == BRIG_OPCODE_MAD || brig_opcode == BRIG_OPCODE_MAD24)
	{
	  // There doesn't seem to be a "standard" MAD built-in in gcc so let's
	  // use a chain of multiply + add for now (double rounding method).
	  // It should be easier for optimizers
	  // than a custom built-in call. WIDEN_MULT_EXPR is close, but
	  // requires a double size result type.
	  tree mult_res
	    = build2 (MULT_EXPR, arith_type, operands[0], operands[1]);
	  return build2 (PLUS_EXPR, arith_type, mult_res, operands[2]);
	}
      else if (brig_opcode == BRIG_OPCODE_MAD24HI)
	{
	  tree mult_res
	    = build2 (MULT_HIGHPART_EXPR, arith_type, operands[0], operands[1]);
	  return build2 (PLUS_EXPR, arith_type, mult_res, operands[2]);
	}
      else if (brig_opcode == BRIG_OPCODE_SHUFFLE)
	{
	  return build_shuffle (arith_type, operands);
	}
      else if (brig_opcode == BRIG_OPCODE_UNPACKLO
	       || brig_opcode == BRIG_OPCODE_UNPACKHI)
	{
	  return build_unpack_lo_or_hi (brig_opcode, arith_type, operands);
	}
      else if (brig_opcode == BRIG_OPCODE_UNPACK)
	{
	  return build_unpack (operands);
	}
      else if (brig_opcode == BRIG_OPCODE_PACK)
	{
	  return build_pack (operands);
	}
      else if (brig_opcode == BRIG_OPCODE_NRSQRT)
	{
	  // Implement as 1.0/sqrt(x) and assume gcc instruction selects to
	  // native ISA other than a division, if available.
	  // TO OPTIMIZE: this will happen only with unsafe math optimizations
	  // on which cannot be used in general for HSAIL compliance. Perhaps
	  // a builtin call would be better option here.
	  return build2 (RDIV_EXPR, arith_type, build_one_cst (arith_type),
			 expand_or_call_builtin (BRIG_OPCODE_SQRT, brig_type,
						 arith_type, operands));
	}
      else if (brig_opcode == BRIG_OPCODE_NRCP)
	{
	  // Implement as 1.0/x and assume gcc instruction selects to
	  // native ISA other than a division, if available.
	  return build2 (RDIV_EXPR, arith_type, build_one_cst (arith_type),
			 operands[0]);
	}
      else if (brig_opcode == BRIG_OPCODE_LANEID
	       || brig_opcode == BRIG_OPCODE_MAXWAVEID
	       || brig_opcode == BRIG_OPCODE_WAVEID)
	{
	  // Assuming WAVESIZE 1 (for now), therefore LANEID, WAVEID and
	  // MAXWAVEID always return 0.
	  return build_zero_cst (arith_type);
	}
      else
	sorry ("Unsupported opcode %d.", brig_opcode);
    }
  else if (opcode == CALL_EXPR)
    return expand_or_call_builtin (brig_opcode, brig_type, arith_type,
				   operands);
  else if (output_count == 1)
    {
      if (input_count == 1)
	{
	  if (opcode == MODIFY_EXPR) // mov
	    return operands[0];
	  else
	    return build1 (opcode, arith_type, operands[0]);
	}
      else if (input_count == 2)
	return build2 (opcode, arith_type, operands[0], operands[1]);
      else if (input_count == 3)
	return build3 (opcode, arith_type, operands[0], operands[1],
		       operands[2]);
      else
	gcc_unreachable ();
    }
  else
    gcc_unreachable ();

  return NULL_TREE;
}

size_t
brig_basic_inst_handler::operator () (const BrigBase *base)
{
  const BrigInstBase *brig_inst = (const BrigInstBase *) base;

  tree_stl_vec operands = build_operands (*brig_inst);

  const int first_input
    = gccbrig_hsa_opcode_op_output_p (brig_inst->opcode, 0) ? 1 : 0;

  size_t output_count = first_input;
  size_t input_count
    = operands.size () == 0 ? 0 : (operands.size () - output_count);

  gcc_assert (output_count == 0 || output_count == 1);

  tree_stl_vec::iterator first_input_i = operands.begin ();
  if (output_count > 0 && operands.size () > 0)
    ++first_input_i;

  tree_stl_vec in_operands;
  in_operands.assign (first_input_i, operands.end ());

  BrigType16_t brig_inst_type = brig_inst->type;

  if (brig_inst->opcode == BRIG_OPCODE_NOP)
    {
      return base->byteCount;
    }
  else if (brig_inst->opcode == BRIG_OPCODE_FIRSTBIT
	   || brig_inst->opcode == BRIG_OPCODE_LASTBIT
	   || brig_inst->opcode == BRIG_OPCODE_SAD)
    {
      // These instructions are reported to be always 32b in HSAIL,
      // but we want to treat them according to their input
      // argument's type to select the correct instruction/builtin.
      brig_inst_type
	= gccbrig_tree_type_to_hsa_type (TREE_TYPE (in_operands[0]));
    }

  tree instr_type = get_tree_type_for_hsa_type (brig_inst_type);

  if (!instr_type)
    {
      internal_error ("unimplemented opcode %u (its tree type is unknown)",
		      brig_inst->opcode);
      return base->byteCount;
    }

  tree_code opcode
    = get_tree_code_for_hsa_opcode (brig_inst->opcode, brig_inst_type);

  bool is_fp16_operation = (brig_inst_type & 0x01F) == BRIG_TYPE_F16
			   && !gccbrig_is_raw_operation (brig_inst->opcode);

  bool is_vec_instr = brig_inst_type & (BRIG_TYPE_PACK_32 | BRIG_TYPE_PACK_64
					| BRIG_TYPE_PACK_128);

  bool scalarize = must_be_scalarized (brig_inst, instr_type);

  size_t element_count;
  size_t element_size_bits;
  if (is_vec_instr)
    {
      BrigType16_t brig_element_type = brig_inst_type & 0x01F;
      element_size_bits = gccbrig_hsa_type_bit_size (brig_element_type);
      element_count = gccbrig_hsa_type_bit_size (brig_inst_type)
		      / gccbrig_hsa_type_bit_size (brig_element_type);
    }
  else
    {
      element_size_bits = gccbrig_hsa_type_bit_size (brig_inst_type);
      element_count = 1;
    }

  // The actual arithmetics type that should be performed with the
  // operation. This is not always the same as the original BRIG
  // opcode's type due to implicit conversions of storage-only f16.
  tree arith_type = gccbrig_is_raw_operation (brig_inst->opcode)
		      ? get_tree_type_for_hsa_type (brig_inst_type)
		      : get_tree_expr_type_for_hsa_type (brig_inst_type);

  tree instr_expr = NULL_TREE;

  BrigPack8_t p = BRIG_PACK_NONE;
  if (brig_inst->base.kind == BRIG_KIND_INST_MOD)
    p = ((const BrigInstMod *) brig_inst)->pack;
  else if (brig_inst->base.kind == BRIG_KIND_INST_CMP)
    p = ((const BrigInstCmp *) brig_inst)->pack;

  if (p == BRIG_PACK_PS || p == BRIG_PACK_PSSAT)
    in_operands[1] = build_lower_element_broadcast (in_operands[1]);
  else if (p == BRIG_PACK_SP || p == BRIG_PACK_SPSAT)
    in_operands[0] = build_lower_element_broadcast (in_operands[0]);

  if (p >= BRIG_PACK_PPSAT && p <= BRIG_PACK_PSAT)
    {
      scalarized_sat_arithmetics sat_arith (*brig_inst);
      gcc_assert (input_count == 2);
      instr_expr = sat_arith (*this, in_operands[0], in_operands[1]);
    }
  else if (opcode == RETURN_EXPR)
    {
      if (m_parent.m_cf->m_is_kernel)
	{
	  tree goto_stmt
	    = build1 (GOTO_EXPR, void_type_node, m_parent.m_cf->m_exit_label);
	  m_parent.m_cf->append_statement (goto_stmt);
	  return base->byteCount;
	}
      else
	{
	  // TO CLEANUP: move to brig_function
	  m_parent.m_cf->append_return_stmt ();
	  return base->byteCount;
	}
    }
  else if (opcode == MULT_HIGHPART_EXPR && scalarize)
    {
      /* MULT_HIGHPART_EXPR works only on target dependent vector sizes and
	 even the scalars do not seem to work at least for char elements.

	 Let's fall back to scalarization and promotion of the vector elements
	 to larger types.

	 This is silly as these type of things do not belong to the frontend,
	 there should be a legalization phase before the backend that figures
	 out the best way to compute this,  but it seems gcc is not modularized
	 in this matter.

	 TO OPTIMIZE: promote to larger vector types instead. For example
	 MULT_HIGHPART_EXPR with  s8x8 doesn't work, but s16x8 seems to at least
	 with my x86-64.
      */
      tree_stl_vec operand0_elements;
      if (input_count > 0)
	unpack (in_operands[0], operand0_elements);

      tree_stl_vec operand1_elements;
      if (input_count > 1)
	unpack (in_operands[1], operand1_elements);

      tree_stl_vec result_elements;

      tree scalar_type = TREE_TYPE (arith_type);
      BrigType16_t element_type = brig_inst_type & 0x01F;
      tree promoted_type = short_integer_type_node;
      switch (element_type)
	{
	default:
	case BRIG_TYPE_S8:
	  promoted_type = short_integer_type_node;
	  break;
	case BRIG_TYPE_U8:
	  promoted_type = short_unsigned_type_node;
	  break;
	case BRIG_TYPE_S16:
	  promoted_type = integer_type_node;
	  break;
	case BRIG_TYPE_U16:
	  promoted_type = unsigned_type_node;
	  break;
	}

      size_t promoted_type_size = int_size_in_bytes (promoted_type) * 8;

      for (size_t i = 0; i < TYPE_VECTOR_SUBPARTS (arith_type); ++i)
	{
	  tree operand0 = convert (promoted_type, operand0_elements.at (i));
	  tree operand1 = convert (promoted_type, operand1_elements.at (i));

	  tree scalar_expr
	    = build2 (MULT_EXPR, promoted_type, operand0, operand1);

	  scalar_expr
	    = build2 (RSHIFT_EXPR, promoted_type, scalar_expr,
		      build_int_cstu (promoted_type, promoted_type_size / 2));

	  result_elements.push_back (convert (scalar_type, scalar_expr));
	}
      instr_expr = pack (result_elements);
    }
  else
    {

      // 'class' is always of b1 type, let's consider it by its
      // float type when building the instruction to find the
      // correct builtin
      if (brig_inst->opcode == BRIG_OPCODE_CLASS)
	brig_inst_type = ((const BrigInstSourceType *) base)->sourceType;
      instr_expr = build_instr_expr (brig_inst->opcode, brig_inst_type,
				     arith_type, in_operands);
    }

  if (instr_expr == NULL_TREE)
    {
      internal_error ("opcode %u %lu inputs %lu outputs", brig_inst->opcode,
		      input_count, output_count);
      return base->byteCount;
    }

  if (p == BRIG_PACK_SS || p == BRIG_PACK_S || p == BRIG_PACK_SSSAT
      || p == BRIG_PACK_SSAT)
    {
      // In case of _s_ or _ss_, select only the lowest element
      // from the new input to the output. We could extract
      // the element and use a scalar operation, but try
      // to keep data in vector registers as much as possible
      // to avoid copies between scalar and vector datapaths.
      tree old_value;
      tree half_storage_type = get_tree_type_for_hsa_type (brig_inst_type);
      if (is_fp16_operation)
	old_value = build_h2f_conversion (
	  build_reinterpret_cast (half_storage_type, operands[0]));
      else
	old_value
	  = build_reinterpret_cast (TREE_TYPE (instr_expr), operands[0]);

      size_t esize = is_fp16_operation ? 32 : element_size_bits;

      // Construct a permutation mask where other elements
      // than the lowest one is picked from the old_value.
      tree mask_inner_type = build_nonstandard_integer_type (esize, 1);
      vec<constructor_elt, va_gc> *constructor_vals = NULL;
      for (size_t i = 0; i < element_count; ++i)
	{
	  tree cst;

	  if (i == 0)
	    cst = build_int_cstu (mask_inner_type, element_count);
	  else
	    cst = build_int_cstu (mask_inner_type, i);
	  CONSTRUCTOR_APPEND_ELT (constructor_vals, NULL_TREE, cst);
	}
      tree mask_vec_type = build_vector_type (mask_inner_type, element_count);
      tree mask = build_vector_from_ctor (mask_vec_type, constructor_vals);

      tree new_value = create_tmp_var (TREE_TYPE (instr_expr), "new_output");
      tree assign
	= build2 (MODIFY_EXPR, TREE_TYPE (instr_expr), new_value, instr_expr);
      m_parent.m_cf->append_statement (assign);

      instr_expr
	= build3 (VEC_PERM_EXPR, arith_type, old_value, new_value, mask);

      tree lower_output = create_tmp_var (TREE_TYPE (instr_expr), "s_output");
      tree assign_lower = build2 (MODIFY_EXPR, TREE_TYPE (instr_expr),
				  lower_output, instr_expr);
      m_parent.m_cf->append_statement (assign_lower);
      instr_expr = lower_output;
    }

  if (output_count == 1)
    build_output_assignment (*brig_inst, operands[0], instr_expr);
  else
    m_parent.m_cf->append_statement (instr_expr);
  return base->byteCount;
}

tree
brig_basic_inst_handler::build_lower_element_broadcast (tree vec_operand)
{
  // Build the broadcast using shuffle because there's no
  // direct broadcast in GENERIC and this way there's no need for
  // a separate extract of the lowest element.
  tree element_type = TREE_TYPE (TREE_TYPE (vec_operand));
  size_t esize = 8 * int_size_in_bytes (element_type);

  size_t element_count = TYPE_VECTOR_SUBPARTS (TREE_TYPE (vec_operand));
  tree mask_inner_type = build_nonstandard_integer_type (esize, 1);
  vec<constructor_elt, va_gc> *constructor_vals = NULL;
  // Construct the mask.
  for (size_t i = 0; i < element_count; ++i)
    {
      tree cst = build_int_cstu (mask_inner_type, element_count);
      CONSTRUCTOR_APPEND_ELT (constructor_vals, NULL_TREE, cst);
    }
  tree mask_vec_type = build_vector_type (mask_inner_type, element_count);
  tree mask = build_vector_from_ctor (mask_vec_type, constructor_vals);

  return build3 (VEC_PERM_EXPR, TREE_TYPE (vec_operand), vec_operand,
		 vec_operand, mask);
}
