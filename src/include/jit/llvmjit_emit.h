/*
 * llvmjit_emit.h
 *	  Helpers to make emitting LLVM IR a bit more concise and pgindent proof.
 *
 * Copyright (c) 2018-2025, PostgreSQL Global Development Group
 *
 * src/include/jit/llvmjit_emit.h
 */
#ifndef LLVMJIT_EMIT_H
#define LLVMJIT_EMIT_H

/*
 * To avoid breaking cpluspluscheck, allow including the file even when LLVM
 * is not available.
 */
#ifdef USE_LLVM

#include <llvm-c/Core.h>
#include <llvm-c/Target.h>

#include "jit/llvmjit.h"


/*
 * Emit a non-LLVM pointer as an LLVM constant.
 */
static inline LLVMValueRef
l_ptr_const(void *ptr, LLVMTypeRef type, LLVMJitTypes *types)
{
	LLVMValueRef c = LLVMConstInt(types->TypeSizeT, (uintptr_t) ptr, false);

	return LLVMConstIntToPtr(c, type);
}

/*
 * Emit pointer.
 */
static inline LLVMTypeRef
l_ptr(LLVMTypeRef t)
{
	return LLVMPointerType(t, 0);
}

/*
 * Emit constant integer.
 */
static inline LLVMValueRef
l_int8_const(LLVMContextRef lc, int8 i)
{
	return LLVMConstInt(LLVMInt8TypeInContext(lc), i, false);
}

/*
 * Emit constant integer.
 */
static inline LLVMValueRef
l_int16_const(LLVMContextRef lc, int16 i)
{
	return LLVMConstInt(LLVMInt16TypeInContext(lc), i, false);
}

/*
 * Emit constant integer.
 */
static inline LLVMValueRef
l_int32_const(LLVMContextRef lc, int32 i)
{
	return LLVMConstInt(LLVMInt32TypeInContext(lc), i, false);
}

/*
 * Emit constant integer.
 */
static inline LLVMValueRef
l_int64_const(LLVMContextRef lc, int64 i)
{
	return LLVMConstInt(LLVMInt64TypeInContext(lc), i, false);
}

/*
 * Emit constant integer.
 */
static inline LLVMValueRef
l_sizet_const(LLVMJitTypes *types, size_t i)
{
	return LLVMConstInt(types->TypeSizeT, i, false);
}

/*
 * Emit constant integer.
 */
static inline LLVMValueRef
l_datum_const(LLVMJitTypes *types, Datum i)
{
	return LLVMConstInt(types->TypeDatum, i, false);
}

/*
 * Emit constant boolean, as used for storage (e.g. global vars, structs).
 */
static inline LLVMValueRef
l_sbool_const(LLVMJitTypes *types, bool i)
{
	return LLVMConstInt(types->TypeStorageBool, (int) i, false);
}

/*
 * Emit constant boolean, as used for parameters (e.g. function parameters).
 */
static inline LLVMValueRef
l_pbool_const(LLVMJitTypes *types, bool i)
{
	return LLVMConstInt(types->TypeParamBool, (int) i, false);
}

static inline LLVMValueRef
l_struct_gep(LLVMBuilderRef b, LLVMTypeRef t, LLVMValueRef v, int32 idx, const char *name)
{
	return LLVMBuildStructGEP2(b, t, v, idx, "");
}


static inline LLVMValueRef
l_gep(LLVMBuilderRef b, LLVMTypeRef t, LLVMValueRef v, LLVMValueRef *indices, int32 nindices, const char *name)
{
	return LLVMBuildGEP2(b, t, v, indices, nindices, name);
}

static inline LLVMValueRef
l_load(LLVMBuilderRef b, LLVMTypeRef t, LLVMValueRef v, const char *name)
{
	return LLVMBuildLoad2(b, t, v, name);
}

/*
 * Load value of a structure using offset of member in structure.
 * b - builder
 * v - value of structure
 * dest - the type of the structure member
 * offset - the offset of the structure member in the structure v
 */
static inline LLVMValueRef
l_load_member_value_by_offset(LLVMBuilderRef b, LLVMContextRef lc, LLVMValueRef v, LLVMTypeRef dest, int offset)
{
    LLVMValueRef offsets = l_int32_const(lc, offset);
    LLVMValueRef member_address = l_gep(b,(LLVMInt8TypeInContext(lc)), v, &offsets, 1, "member_address");
    return l_load(b, dest, member_address, "member_value");
}

#define l_ptr_const_step(m,t) l_load_member_value_by_offset(b,lc, v_op, t, offsetof(ExprEvalStep ,m))


static inline LLVMValueRef
l_struct_member_ptr_by_offset(LLVMBuilderRef b, LLVMContextRef lc, LLVMValueRef v, int offset)
{
    LLVMValueRef offsets = l_int32_const(lc, offset);
    LLVMValueRef member_address = l_gep(b,(LLVMInt8TypeInContext(lc)), v, &offsets, 1, "member_address");
    return member_address;
}

static inline LLVMValueRef
l_call(LLVMBuilderRef b, LLVMTypeRef t, LLVMValueRef fn, LLVMValueRef *args, int32 nargs, const char *name)
{
	return LLVMBuildCall2(b, t, fn, args, nargs, name);
}

/*
 * Load a pointer member idx from a struct.
 */
static inline LLVMValueRef
l_load_struct_gep(LLVMBuilderRef b, LLVMTypeRef t, LLVMValueRef v, int32 idx, const char *name)
{
	return l_load(b,
				  LLVMStructGetTypeAtIndex(t, idx),
				  l_struct_gep(b, t, v, idx, ""),
				  name);
}

/*
 * Load value of a pointer, after applying one index operation.
 */
static inline LLVMValueRef
l_load_gep1(LLVMBuilderRef b, LLVMTypeRef t, LLVMValueRef v, LLVMValueRef idx, const char *name)
{
	return l_load(b, t, l_gep(b, t, v, &idx, 1, ""), name);
}

/* separate, because pg_attribute_printf(2, 3) can't appear in definition */
static inline LLVMBasicBlockRef l_bb_before_v(LLVMBasicBlockRef r, const char *fmt,...) pg_attribute_printf(2, 3);

/*
 * Insert a new basic block, just before r, the name being determined by fmt
 * and arguments.
 */
static inline LLVMBasicBlockRef
l_bb_before_v(LLVMBasicBlockRef r, const char *fmt,...)
{
	char		buf[512];
	va_list		args;
	LLVMContextRef lc;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	lc = LLVMGetTypeContext(LLVMTypeOf(LLVMGetBasicBlockParent(r)));

	return LLVMInsertBasicBlockInContext(lc, r, buf);
}

/* separate, because pg_attribute_printf(2, 3) can't appear in definition */
static inline LLVMBasicBlockRef l_bb_append_v(LLVMValueRef f, const char *fmt,...) pg_attribute_printf(2, 3);

/*
 * Insert a new basic block after previous basic blocks, the name being
 * determined by fmt and arguments.
 */
static inline LLVMBasicBlockRef
l_bb_append_v(LLVMValueRef f, const char *fmt,...)
{
	char		buf[512];
	va_list		args;
	LLVMContextRef lc;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	lc = LLVMGetTypeContext(LLVMTypeOf(f));

	return LLVMAppendBasicBlockInContext(lc, f, buf);
}

/*
 * Mark a callsite as readonly.
 */
static inline void
l_callsite_ro(LLVMValueRef f)
{
	const char	argname[] = "readonly";
	LLVMAttributeRef ref;

	ref = LLVMCreateStringAttribute(LLVMGetTypeContext(LLVMTypeOf(f)),
									argname,
									sizeof(argname) - 1,
									NULL, 0);

	LLVMAddCallSiteAttribute(f, LLVMAttributeFunctionIndex, ref);
}

/*
 * Mark a callsite as alwaysinline.
 */
static inline void
l_callsite_alwaysinline(LLVMValueRef f)
{
	const char	argname[] = "alwaysinline";
	int			id;
	LLVMAttributeRef attr;

	id = LLVMGetEnumAttributeKindForName(argname,
										 sizeof(argname) - 1);
	attr = LLVMCreateEnumAttribute(LLVMGetTypeContext(LLVMTypeOf(f)), id, 0);
	LLVMAddCallSiteAttribute(f, LLVMAttributeFunctionIndex, attr);
}

/*
 * Emit code to switch memory context.
 */
static inline LLVMValueRef
l_mcxt_switch(LLVMModuleRef mod, LLVMBuilderRef b, LLVMValueRef nc, LLVMJitTypes *types)
{
	const char *cmc = "CurrentMemoryContext";
	LLVMValueRef cur;
	LLVMValueRef ret;

	if (!(cur = LLVMGetNamedGlobal(mod, cmc)))
		cur = LLVMAddGlobal(mod, l_ptr(types->StructMemoryContextData), cmc);
	ret = l_load(b, l_ptr(types->StructMemoryContextData), cur, cmc);
	LLVMBuildStore(b, nc, cur);

	return ret;
}

/*
 * Return pointer to the argno'th argument nullness.
 */
static inline LLVMValueRef
l_funcnullp(LLVMBuilderRef b, LLVMValueRef v_fcinfo, size_t argno, LLVMJitTypes *types)
{
	LLVMValueRef v_args;
	LLVMValueRef v_argn;

	v_args = l_struct_gep(b,
						  types->StructFunctionCallInfoData,
						  v_fcinfo,
						  FIELDNO_FUNCTIONCALLINFODATA_ARGS,
						  "");
	v_argn = l_struct_gep(b,
						  LLVMArrayType(types->StructNullableDatum, 0),
						  v_args,
						  argno,
						  "");
	return l_struct_gep(b,
						types->StructNullableDatum,
						v_argn,
						FIELDNO_NULLABLE_DATUM_ISNULL,
						"");
}

/*
 * Return pointer to the argno'th argument datum.
 */
static inline LLVMValueRef
l_funcvaluep(LLVMBuilderRef b, LLVMValueRef v_fcinfo, size_t argno, LLVMJitTypes *types)
{
	LLVMValueRef v_args;
	LLVMValueRef v_argn;

	v_args = l_struct_gep(b,
						  types->StructFunctionCallInfoData,
						  v_fcinfo,
						  FIELDNO_FUNCTIONCALLINFODATA_ARGS,
						  "");
	v_argn = l_struct_gep(b,
						  LLVMArrayType(types->StructNullableDatum, 0),
						  v_args,
						  argno,
						  "");
	return l_struct_gep(b,
						types->StructNullableDatum,
						v_argn,
						FIELDNO_NULLABLE_DATUM_DATUM,
						"");
}

/*
 * Return argno'th argument nullness.
 */
static inline LLVMValueRef
l_funcnull(LLVMBuilderRef b, LLVMValueRef v_fcinfo, size_t argno, LLVMJitTypes *types)
{
	return l_load(b, types->TypeStorageBool, l_funcnullp(b, v_fcinfo, argno, types), "");
}

/*
 * Return argno'th argument datum.
 */
static inline LLVMValueRef
l_funcvalue(LLVMBuilderRef b, LLVMValueRef v_fcinfo, size_t argno, LLVMJitTypes *types)
{
	return l_load(b, types->TypeDatum, l_funcvaluep(b, v_fcinfo, argno, types), "");
}

#endif							/* USE_LLVM */
#endif
