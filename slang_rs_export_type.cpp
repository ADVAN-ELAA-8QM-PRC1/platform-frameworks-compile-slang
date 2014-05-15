/*
 * Copyright 2010-2012, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "slang_rs_export_type.h"

#include <list>
#include <vector>

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/RecordLayout.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Type.h"

#include "slang_assert.h"
#include "slang_rs_context.h"
#include "slang_rs_export_element.h"
#include "slang_version.h"

#define CHECK_PARENT_EQUALITY(ParentClass, E) \
  if (!ParentClass::equals(E))                \
    return false;

namespace slang {

namespace {

/* For the data types we support, their category, names, and size (in bits).
 *
 * IMPORTANT: The data types in this table should be at the same index
 * as specified by the corresponding DataType enum.
 */
static RSReflectionType gReflectionTypes[] = {
    {PrimitiveDataType, "FLOAT_16", "F16", 16, "half", "half", "Half", "Half", false},
    {PrimitiveDataType, "FLOAT_32", "F32", 32, "float", "float", "Float", "Float", false},
    {PrimitiveDataType, "FLOAT_64", "F64", 64, "double", "double", "Double", "Double",false},
    {PrimitiveDataType, "SIGNED_8", "I8", 8, "int8_t", "byte", "Byte", "Byte", false},
    {PrimitiveDataType, "SIGNED_16", "I16", 16, "int16_t", "short", "Short", "Short", false},
    {PrimitiveDataType, "SIGNED_32", "I32", 32, "int32_t", "int", "Int", "Int", false},
    {PrimitiveDataType, "SIGNED_64", "I64", 64, "int64_t", "long", "Long", "Long", false},
    {PrimitiveDataType, "UNSIGNED_8", "U8", 8, "uint8_t", "short", "UByte", "Short", true},
    {PrimitiveDataType, "UNSIGNED_16", "U16", 16, "uint16_t", "int", "UShort", "Int", true},
    {PrimitiveDataType, "UNSIGNED_32", "U32", 32, "uint32_t", "long", "UInt", "Long", true},
    {PrimitiveDataType, "UNSIGNED_64", "U64", 64, "uint64_t", "long", "ULong", "Long", false},

    {PrimitiveDataType, "BOOLEAN", "BOOLEAN", 8, "bool", "boolean", NULL, NULL, false},

    {PrimitiveDataType, "UNSIGNED_5_6_5", NULL, 16, NULL, NULL, NULL, NULL, false},
    {PrimitiveDataType, "UNSIGNED_5_5_5_1", NULL, 16, NULL, NULL, NULL, NULL, false},
    {PrimitiveDataType, "UNSIGNED_4_4_4_4", NULL, 16, NULL, NULL, NULL, NULL, false},

    {MatrixDataType, "MATRIX_2X2", NULL, 4*32, "rsMatrix_2x2", "Matrix2f", NULL, NULL, false},
    {MatrixDataType, "MATRIX_3X3", NULL, 9*32, "rsMatrix_3x3", "Matrix3f", NULL, NULL, false},
    {MatrixDataType, "MATRIX_4X4", NULL, 16*32, "rsMatrix_4x4", "Matrix4f", NULL, NULL, false},

    // TODO: For 64 bit, what will be the size of the objects??
    {ObjectDataType, "RS_ELEMENT", "ELEMENT", 32, "Element", "Element", NULL, NULL, false},
    {ObjectDataType, "RS_TYPE", "TYPE", 32, "Type", "Type", NULL, NULL, false},
    {ObjectDataType, "RS_ALLOCATION", "ALLOCATION", 32, "Allocation", "Allocation", NULL, NULL, false},
    {ObjectDataType, "RS_SAMPLER", "SAMPLER", 32, "Sampler", "Sampler", NULL, NULL, false},
    {ObjectDataType, "RS_SCRIPT", "SCRIPT", 32, "Script", "Script", NULL, NULL, false},
    {ObjectDataType, "RS_MESH", "MESH", 32, "Mesh", "Mesh", NULL, NULL, false},
    {ObjectDataType, "RS_PATH", "PATH", 32, "Path", "Path", NULL, NULL, false},

    {ObjectDataType, "RS_PROGRAM_FRAGMENT", "PROGRAM_FRAGMENT", 32, "ProgramFragment", "ProgramFragment", NULL, NULL, false},
    {ObjectDataType, "RS_PROGRAM_VERTEX", "PROGRAM_VERTEX", 32, "ProgramVertex", "ProgramVertex", NULL, NULL, false},
    {ObjectDataType, "RS_PROGRAM_RASTER", "PROGRAM_RASTER", 32, "ProgramRaster", "ProgramRaster", NULL, NULL, false},
    {ObjectDataType, "RS_PROGRAM_STORE", "PROGRAM_STORE", 32, "ProgramStore", "ProgramStore", NULL, NULL, false},
    {ObjectDataType, "RS_FONT", "FONT", 32, "Font", "Font", NULL, NULL, false}
};

const int kMaxVectorSize = 4;

struct BuiltinInfo {
  clang::BuiltinType::Kind builtinTypeKind;
  RSExportPrimitiveType::DataType type;
  /* TODO If we return std::string instead of llvm::StringRef, we could build
   * the name instead of duplicating the entries.
   */
  const char *cname[kMaxVectorSize];
};

BuiltinInfo BuiltinInfoTable[] = {
    {clang::BuiltinType::Bool, RSExportPrimitiveType::DataTypeBoolean,
     {"bool", "bool2", "bool3", "bool4"}},
    {clang::BuiltinType::Char_U, RSExportPrimitiveType::DataTypeUnsigned8,
     {"uchar", "uchar2", "uchar3", "uchar4"}},
    {clang::BuiltinType::UChar, RSExportPrimitiveType::DataTypeUnsigned8,
     {"uchar", "uchar2", "uchar3", "uchar4"}},
    {clang::BuiltinType::Char16, RSExportPrimitiveType::DataTypeSigned16,
     {"short", "short2", "short3", "short4"}},
    {clang::BuiltinType::Char32, RSExportPrimitiveType::DataTypeSigned32,
     {"int", "int2", "int3", "int4"}},
    {clang::BuiltinType::UShort, RSExportPrimitiveType::DataTypeUnsigned16,
     {"ushort", "ushort2", "ushort3", "ushort4"}},
    {clang::BuiltinType::UInt, RSExportPrimitiveType::DataTypeUnsigned32,
     {"uint", "uint2", "uint3", "uint4"}},
    {clang::BuiltinType::ULong, RSExportPrimitiveType::DataTypeUnsigned32,
     {"uint", "uint2", "uint3", "uint4"}},
    {clang::BuiltinType::ULongLong, RSExportPrimitiveType::DataTypeUnsigned64,
     {"ulong", "ulong2", "ulong3", "ulong4"}},

    {clang::BuiltinType::Char_S, RSExportPrimitiveType::DataTypeSigned8,
     {"char", "char2", "char3", "char4"}},
    {clang::BuiltinType::SChar, RSExportPrimitiveType::DataTypeSigned8,
     {"char", "char2", "char3", "char4"}},
    {clang::BuiltinType::Short, RSExportPrimitiveType::DataTypeSigned16,
     {"short", "short2", "short3", "short4"}},
    {clang::BuiltinType::Int, RSExportPrimitiveType::DataTypeSigned32,
     {"int", "int2", "int3", "int4"}},
    {clang::BuiltinType::Long, RSExportPrimitiveType::DataTypeSigned64,
     {"long", "long2", "long3", "long4"}},
    {clang::BuiltinType::LongLong, RSExportPrimitiveType::DataTypeSigned64,
     {"long", "long2", "long3", "long4"}},
    {clang::BuiltinType::Float, RSExportPrimitiveType::DataTypeFloat32,
     {"float", "float2", "float3", "float4"}},
    {clang::BuiltinType::Double, RSExportPrimitiveType::DataTypeFloat64,
     {"double", "double2", "double3", "double4"}},
};
const int BuiltinInfoTableCount = sizeof(BuiltinInfoTable) / sizeof(BuiltinInfoTable[0]);

struct NameAndPrimitiveType {
  const char *name;
  RSExportPrimitiveType::DataType dataType;
};

static NameAndPrimitiveType MatrixAndObjectDataTypes[] = {
    {"rs_matrix2x2", RSExportPrimitiveType::DataTypeRSMatrix2x2},
    {"rs_matrix3x3", RSExportPrimitiveType::DataTypeRSMatrix3x3},
    {"rs_matrix4x4", RSExportPrimitiveType::DataTypeRSMatrix4x4},
    {"rs_element", RSExportPrimitiveType::DataTypeRSElement},
    {"rs_type", RSExportPrimitiveType::DataTypeRSType},
    {"rs_allocation", RSExportPrimitiveType::DataTypeRSAllocation},
    {"rs_sampler", RSExportPrimitiveType::DataTypeRSSampler},
    {"rs_script", RSExportPrimitiveType::DataTypeRSScript},
    {"rs_mesh", RSExportPrimitiveType::DataTypeRSMesh},
    {"rs_path", RSExportPrimitiveType::DataTypeRSPath},
    {"rs_program_fragment", RSExportPrimitiveType::DataTypeRSProgramFragment},
    {"rs_program_vertex", RSExportPrimitiveType::DataTypeRSProgramVertex},
    {"rs_program_raster", RSExportPrimitiveType::DataTypeRSProgramRaster},
    {"rs_program_store", RSExportPrimitiveType::DataTypeRSProgramStore},
    {"rs_font", RSExportPrimitiveType::DataTypeRSFont},
};

const int MatrixAndObjectDataTypesCount =
    sizeof(MatrixAndObjectDataTypes) / sizeof(MatrixAndObjectDataTypes[0]);

static const clang::Type *TypeExportableHelper(
    const clang::Type *T,
    llvm::SmallPtrSet<const clang::Type*, 8>& SPS,
    slang::RSContext *Context,
    const clang::VarDecl *VD,
    const clang::RecordDecl *TopLevelRecord);

template <unsigned N>
static void ReportTypeError(slang::RSContext *Context,
                            const clang::NamedDecl *ND,
                            const clang::RecordDecl *TopLevelRecord,
                            const char (&Message)[N],
                            unsigned int TargetAPI = 0) {
  // Attempt to use the type declaration first (if we have one).
  // Fall back to the variable definition, if we are looking at something
  // like an array declaration that can't be exported.
  if (TopLevelRecord) {
    Context->ReportError(TopLevelRecord->getLocation(), Message)
        << TopLevelRecord->getName() << TargetAPI;
  } else if (ND) {
    Context->ReportError(ND->getLocation(), Message) << ND->getName()
                                                     << TargetAPI;
  } else {
    slangAssert(false && "Variables should be validated before exporting");
  }
}

static const clang::Type *ConstantArrayTypeExportableHelper(
    const clang::ConstantArrayType *CAT,
    llvm::SmallPtrSet<const clang::Type*, 8>& SPS,
    slang::RSContext *Context,
    const clang::VarDecl *VD,
    const clang::RecordDecl *TopLevelRecord) {
  // Check element type
  const clang::Type *ElementType = GET_CONSTANT_ARRAY_ELEMENT_TYPE(CAT);
  if (ElementType->isArrayType()) {
    ReportTypeError(Context, VD, TopLevelRecord,
                    "multidimensional arrays cannot be exported: '%0'");
    return NULL;
  } else if (ElementType->isExtVectorType()) {
    const clang::ExtVectorType *EVT =
        static_cast<const clang::ExtVectorType*>(ElementType);
    unsigned numElements = EVT->getNumElements();

    const clang::Type *BaseElementType = GET_EXT_VECTOR_ELEMENT_TYPE(EVT);
    if (!RSExportPrimitiveType::IsPrimitiveType(BaseElementType)) {
      ReportTypeError(Context, VD, TopLevelRecord,
        "vectors of non-primitive types cannot be exported: '%0'");
      return NULL;
    }

    if (numElements == 3 && CAT->getSize() != 1) {
      ReportTypeError(Context, VD, TopLevelRecord,
        "arrays of width 3 vector types cannot be exported: '%0'");
      return NULL;
    }
  }

  if (TypeExportableHelper(ElementType, SPS, Context, VD,
                           TopLevelRecord) == NULL) {
    return NULL;
  } else {
    return CAT;
  }
}

BuiltinInfo *FindBuiltinType(clang::BuiltinType::Kind builtinTypeKind) {
  for (int i = 0; i < BuiltinInfoTableCount; i++) {
    if (builtinTypeKind == BuiltinInfoTable[i].builtinTypeKind) {
      return &BuiltinInfoTable[i];
    }
  }
  return NULL;
}

static const clang::Type *TypeExportableHelper(
    clang::Type const *T,
    llvm::SmallPtrSet<clang::Type const *, 8> &SPS,
    slang::RSContext *Context,
    clang::VarDecl const *VD,
    clang::RecordDecl const *TopLevelRecord) {
  // Normalize first
  if ((T = GET_CANONICAL_TYPE(T)) == NULL)
    return NULL;

  if (SPS.count(T))
    return T;

  switch (T->getTypeClass()) {
    case clang::Type::Builtin: {
      const clang::BuiltinType *BT =
        UNSAFE_CAST_TYPE(const clang::BuiltinType, T);
      return FindBuiltinType(BT->getKind()) == NULL ? NULL : T;
    }
    case clang::Type::Record: {
      if (RSExportPrimitiveType::GetRSSpecificType(T) !=
          RSExportPrimitiveType::DataTypeUnknown) {
        return T;  // RS object type, no further checks are needed
      }

      // Check internal struct
      if (T->isUnionType()) {
        ReportTypeError(Context, VD, T->getAsUnionType()->getDecl(),
                        "unions cannot be exported: '%0'");
        return NULL;
      } else if (!T->isStructureType()) {
        slangAssert(false && "Unknown type cannot be exported");
        return NULL;
      }

      clang::RecordDecl *RD = T->getAsStructureType()->getDecl();
      if (RD != NULL) {
        RD = RD->getDefinition();
        if (RD == NULL) {
          ReportTypeError(Context, NULL, T->getAsStructureType()->getDecl(),
                          "struct is not defined in this module");
          return NULL;
        }
      }

      if (!TopLevelRecord) {
        TopLevelRecord = RD;
      }
      if (RD->getName().empty()) {
        ReportTypeError(Context, NULL, RD,
                        "anonymous structures cannot be exported");
        return NULL;
      }

      // Fast check
      if (RD->hasFlexibleArrayMember() || RD->hasObjectMember())
        return NULL;

      // Insert myself into checking set
      SPS.insert(T);

      // Check all element
      for (clang::RecordDecl::field_iterator FI = RD->field_begin(),
               FE = RD->field_end();
           FI != FE;
           FI++) {
        const clang::FieldDecl *FD = *FI;
        const clang::Type *FT = RSExportType::GetTypeOfDecl(FD);
        FT = GET_CANONICAL_TYPE(FT);

        if (!TypeExportableHelper(FT, SPS, Context, VD, TopLevelRecord)) {
          return NULL;
        }

        // We don't support bit fields yet
        //
        // TODO(zonr/srhines): allow bit fields of size 8, 16, 32
        if (FD->isBitField()) {
          Context->ReportError(
              FD->getLocation(),
              "bit fields are not able to be exported: '%0.%1'")
              << RD->getName() << FD->getName();
          return NULL;
        }
      }

      return T;
    }
    case clang::Type::Pointer: {
      if (TopLevelRecord) {
        ReportTypeError(Context, VD, TopLevelRecord,
            "structures containing pointers cannot be exported: '%0'");
        return NULL;
      }

      const clang::PointerType *PT =
        UNSAFE_CAST_TYPE(const clang::PointerType, T);
      const clang::Type *PointeeType = GET_POINTEE_TYPE(PT);

      if (PointeeType->getTypeClass() == clang::Type::Pointer) {
        ReportTypeError(Context, VD, TopLevelRecord,
            "multiple levels of pointers cannot be exported: '%0'");
        return NULL;
      }
      // We don't support pointer with array-type pointee or unsupported pointee
      // type
      if (PointeeType->isArrayType() ||
          (TypeExportableHelper(PointeeType, SPS, Context, VD,
                                TopLevelRecord) == NULL))
        return NULL;
      else
        return T;
    }
    case clang::Type::ExtVector: {
      const clang::ExtVectorType *EVT =
          UNSAFE_CAST_TYPE(const clang::ExtVectorType, T);
      // Only vector with size 2, 3 and 4 are supported.
      if (EVT->getNumElements() < 2 || EVT->getNumElements() > 4)
        return NULL;

      // Check base element type
      const clang::Type *ElementType = GET_EXT_VECTOR_ELEMENT_TYPE(EVT);

      if ((ElementType->getTypeClass() != clang::Type::Builtin) ||
          (TypeExportableHelper(ElementType, SPS, Context, VD,
                                TopLevelRecord) == NULL))
        return NULL;
      else
        return T;
    }
    case clang::Type::ConstantArray: {
      const clang::ConstantArrayType *CAT =
          UNSAFE_CAST_TYPE(const clang::ConstantArrayType, T);

      return ConstantArrayTypeExportableHelper(CAT, SPS, Context, VD,
                                               TopLevelRecord);
    }
    case clang::Type::Enum: {
      // FIXME: We currently convert enums to integers, rather than reflecting
      // a more complete (and nicer type-safe Java version).
      return Context->getASTContext().IntTy.getTypePtr();
    }
    default: {
      slangAssert(false && "Unknown type cannot be validated");
      return NULL;
    }
  }
}

// Return the type that can be used to create RSExportType, will always return
// the canonical type
// If the Type T is not exportable, this function returns NULL. DiagEngine is
// used to generate proper Clang diagnostic messages when a
// non-exportable type is detected. TopLevelRecord is used to capture the
// highest struct (in the case of a nested hierarchy) for detecting other
// types that cannot be exported (mostly pointers within a struct).
static const clang::Type *TypeExportable(const clang::Type *T,
                                         slang::RSContext *Context,
                                         const clang::VarDecl *VD) {
  llvm::SmallPtrSet<const clang::Type*, 8> SPS =
      llvm::SmallPtrSet<const clang::Type*, 8>();

  return TypeExportableHelper(T, SPS, Context, VD, NULL);
}

static bool ValidateRSObjectInVarDecl(slang::RSContext *Context,
                                      clang::VarDecl *VD, bool InCompositeType,
                                      unsigned int TargetAPI) {
  if (TargetAPI < SLANG_JB_TARGET_API) {
    // Only if we are already in a composite type (like an array or structure).
    if (InCompositeType) {
      // Only if we are actually exported (i.e. non-static).
      if (VD->hasLinkage() &&
          (VD->getFormalLinkage() == clang::ExternalLinkage)) {
        // Only if we are not a pointer to an object.
        const clang::Type *T = GET_CANONICAL_TYPE(VD->getType().getTypePtr());
        if (T->getTypeClass() != clang::Type::Pointer) {
          ReportTypeError(Context, VD, NULL,
                          "arrays/structures containing RS object types "
                          "cannot be exported in target API < %1: '%0'",
                          SLANG_JB_TARGET_API);
          return false;
        }
      }
    }
  }

  return true;
}

// Helper function for ValidateType(). We do a recursive descent on the
// type hierarchy to ensure that we can properly export/handle the
// declaration.
// \return true if the variable declaration is valid,
//         false if it is invalid (along with proper diagnostics).
//
// C - ASTContext (for diagnostics + builtin types).
// T - sub-type that we are validating.
// ND - (optional) top-level named declaration that we are validating.
// SPS - set of types we have already seen/validated.
// InCompositeType - true if we are within an outer composite type.
// UnionDecl - set if we are in a sub-type of a union.
// TargetAPI - target SDK API level.
// IsFilterscript - whether or not we are compiling for Filterscript
static bool ValidateTypeHelper(
    slang::RSContext *Context,
    clang::ASTContext &C,
    const clang::Type *&T,
    clang::NamedDecl *ND,
    clang::SourceLocation Loc,
    llvm::SmallPtrSet<const clang::Type*, 8>& SPS,
    bool InCompositeType,
    clang::RecordDecl *UnionDecl,
    unsigned int TargetAPI,
    bool IsFilterscript) {
  if ((T = GET_CANONICAL_TYPE(T)) == NULL)
    return true;

  if (SPS.count(T))
    return true;

  switch (T->getTypeClass()) {
    case clang::Type::Record: {
      if (RSExportPrimitiveType::IsRSObjectType(T)) {
        clang::VarDecl *VD = (ND ? llvm::dyn_cast<clang::VarDecl>(ND) : NULL);
        if (VD && !ValidateRSObjectInVarDecl(Context, VD, InCompositeType,
                                             TargetAPI)) {
          return false;
        }
      }

      if (RSExportPrimitiveType::GetRSSpecificType(T) !=
          RSExportPrimitiveType::DataTypeUnknown) {
        if (!UnionDecl) {
          return true;
        } else if (RSExportPrimitiveType::IsRSObjectType(T)) {
          ReportTypeError(Context, NULL, UnionDecl,
              "unions containing RS object types are not allowed");
          return false;
        }
      }

      clang::RecordDecl *RD = NULL;

      // Check internal struct
      if (T->isUnionType()) {
        RD = T->getAsUnionType()->getDecl();
        UnionDecl = RD;
      } else if (T->isStructureType()) {
        RD = T->getAsStructureType()->getDecl();
      } else {
        slangAssert(false && "Unknown type cannot be exported");
        return false;
      }

      if (RD != NULL) {
        RD = RD->getDefinition();
        if (RD == NULL) {
          // FIXME
          return true;
        }
      }

      // Fast check
      if (RD->hasFlexibleArrayMember() || RD->hasObjectMember())
        return false;

      // Insert myself into checking set
      SPS.insert(T);

      // Check all elements
      for (clang::RecordDecl::field_iterator FI = RD->field_begin(),
               FE = RD->field_end();
           FI != FE;
           FI++) {
        const clang::FieldDecl *FD = *FI;
        const clang::Type *FT = RSExportType::GetTypeOfDecl(FD);
        FT = GET_CANONICAL_TYPE(FT);

        if (!ValidateTypeHelper(Context, C, FT, ND, Loc, SPS, true, UnionDecl,
                                TargetAPI, IsFilterscript)) {
          return false;
        }
      }

      return true;
    }

    case clang::Type::Builtin: {
      if (IsFilterscript) {
        clang::QualType QT = T->getCanonicalTypeInternal();
        if (QT == C.DoubleTy ||
            QT == C.LongDoubleTy ||
            QT == C.LongTy ||
            QT == C.LongLongTy) {
          if (ND) {
            Context->ReportError(
                Loc,
                "Builtin types > 32 bits in size are forbidden in "
                "Filterscript: '%0'")
                << ND->getName();
          } else {
            Context->ReportError(
                Loc,
                "Builtin types > 32 bits in size are forbidden in "
                "Filterscript");
          }
          return false;
        }
      }
      break;
    }

    case clang::Type::Pointer: {
      if (IsFilterscript) {
        if (ND) {
          Context->ReportError(Loc,
                               "Pointers are forbidden in Filterscript: '%0'")
              << ND->getName();
          return false;
        } else {
          // TODO(srhines): Find a better way to handle expressions (i.e. no
          // NamedDecl) involving pointers in FS that should be allowed.
          // An example would be calls to library functions like
          // rsMatrixMultiply() that take rs_matrixNxN * types.
        }
      }

      const clang::PointerType *PT =
        UNSAFE_CAST_TYPE(const clang::PointerType, T);
      const clang::Type *PointeeType = GET_POINTEE_TYPE(PT);

      return ValidateTypeHelper(Context, C, PointeeType, ND, Loc, SPS,
                                InCompositeType, UnionDecl, TargetAPI,
                                IsFilterscript);
    }

    case clang::Type::ExtVector: {
      const clang::ExtVectorType *EVT =
          UNSAFE_CAST_TYPE(const clang::ExtVectorType, T);
      const clang::Type *ElementType = GET_EXT_VECTOR_ELEMENT_TYPE(EVT);
      if (TargetAPI < SLANG_ICS_TARGET_API &&
          InCompositeType &&
          EVT->getNumElements() == 3 &&
          ND &&
          ND->getFormalLinkage() == clang::ExternalLinkage) {
        ReportTypeError(Context, ND, NULL,
                        "structs containing vectors of dimension 3 cannot "
                        "be exported at this API level: '%0'");
        return false;
      }
      return ValidateTypeHelper(Context, C, ElementType, ND, Loc, SPS, true,
                                UnionDecl, TargetAPI, IsFilterscript);
    }

    case clang::Type::ConstantArray: {
      const clang::ConstantArrayType *CAT =
          UNSAFE_CAST_TYPE(const clang::ConstantArrayType, T);
      const clang::Type *ElementType = GET_CONSTANT_ARRAY_ELEMENT_TYPE(CAT);
      return ValidateTypeHelper(Context, C, ElementType, ND, Loc, SPS, true,
                                UnionDecl, TargetAPI, IsFilterscript);
    }

    default: {
      break;
    }
  }

  return true;
}

}  // namespace

/****************************** RSExportType ******************************/
bool RSExportType::NormalizeType(const clang::Type *&T,
                                 llvm::StringRef &TypeName,
                                 RSContext *Context,
                                 const clang::VarDecl *VD) {
  if ((T = TypeExportable(T, Context, VD)) == NULL) {
    return false;
  }
  // Get type name
  TypeName = RSExportType::GetTypeName(T);
  if (Context && TypeName.empty()) {
    if (VD) {
      Context->ReportError(VD->getLocation(),
                           "anonymous types cannot be exported");
    } else {
      Context->ReportError("anonymous types cannot be exported");
    }
    return false;
  }

  return true;
}

bool RSExportType::ValidateType(slang::RSContext *Context, clang::ASTContext &C,
                                clang::QualType QT, clang::NamedDecl *ND,
                                clang::SourceLocation Loc,
                                unsigned int TargetAPI, bool IsFilterscript) {
  const clang::Type *T = QT.getTypePtr();
  llvm::SmallPtrSet<const clang::Type*, 8> SPS =
      llvm::SmallPtrSet<const clang::Type*, 8>();

  return ValidateTypeHelper(Context, C, T, ND, Loc, SPS, false, NULL, TargetAPI,
                            IsFilterscript);
  return true;
}

bool RSExportType::ValidateVarDecl(slang::RSContext *Context,
                                   clang::VarDecl *VD, unsigned int TargetAPI,
                                   bool IsFilterscript) {
  return ValidateType(Context, VD->getASTContext(), VD->getType(), VD,
                      VD->getLocation(), TargetAPI, IsFilterscript);
}

const clang::Type
*RSExportType::GetTypeOfDecl(const clang::DeclaratorDecl *DD) {
  if (DD) {
    clang::QualType T = DD->getType();

    if (T.isNull())
      return NULL;
    else
      return T.getTypePtr();
  }
  return NULL;
}

llvm::StringRef RSExportType::GetTypeName(const clang::Type* T) {
  T = GET_CANONICAL_TYPE(T);
  if (T == NULL)
    return llvm::StringRef();

  switch (T->getTypeClass()) {
    case clang::Type::Builtin: {
      const clang::BuiltinType *BT =
        UNSAFE_CAST_TYPE(const clang::BuiltinType, T);
      BuiltinInfo *info = FindBuiltinType(BT->getKind());
      if (info != NULL) {
        return info->cname[0];
      }
      slangAssert(false && "Unknown data type of the builtin");
      break;
    }
    case clang::Type::Record: {
      clang::RecordDecl *RD;
      if (T->isStructureType()) {
        RD = T->getAsStructureType()->getDecl();
      } else {
        break;
      }

      llvm::StringRef Name = RD->getName();
      if (Name.empty()) {
        if (RD->getTypedefNameForAnonDecl() != NULL) {
          Name = RD->getTypedefNameForAnonDecl()->getName();
        }

        if (Name.empty()) {
          // Try to find a name from redeclaration (i.e. typedef)
          for (clang::TagDecl::redecl_iterator RI = RD->redecls_begin(),
                   RE = RD->redecls_end();
               RI != RE;
               RI++) {
            slangAssert(*RI != NULL && "cannot be NULL object");

            Name = (*RI)->getName();
            if (!Name.empty())
              break;
          }
        }
      }
      return Name;
    }
    case clang::Type::Pointer: {
      // "*" plus pointee name
      const clang::Type *PT = GET_POINTEE_TYPE(T);
      llvm::StringRef PointeeName;
      if (NormalizeType(PT, PointeeName, NULL, NULL)) {
        char *Name = new char[ 1 /* * */ + PointeeName.size() + 1 ];
        Name[0] = '*';
        memcpy(Name + 1, PointeeName.data(), PointeeName.size());
        Name[PointeeName.size() + 1] = '\0';
        return Name;
      }
      break;
    }
    case clang::Type::ExtVector: {
      const clang::ExtVectorType *EVT =
          UNSAFE_CAST_TYPE(const clang::ExtVectorType, T);
      return RSExportVectorType::GetTypeName(EVT);
      break;
    }
    case clang::Type::ConstantArray : {
      // Construct name for a constant array is too complicated.
      return DUMMY_TYPE_NAME_FOR_RS_CONSTANT_ARRAY_TYPE;
    }
    default: {
      break;
    }
  }

  return llvm::StringRef();
}


RSExportType *RSExportType::Create(RSContext *Context,
                                   const clang::Type *T,
                                   const llvm::StringRef &TypeName) {
  // Lookup the context to see whether the type was processed before.
  // Newly created RSExportType will insert into context
  // in RSExportType::RSExportType()
  RSContext::export_type_iterator ETI = Context->findExportType(TypeName);

  if (ETI != Context->export_types_end())
    return ETI->second;

  RSExportType *ET = NULL;
  switch (T->getTypeClass()) {
    case clang::Type::Record: {
      RSExportPrimitiveType::DataType dt =
          RSExportPrimitiveType::GetRSSpecificType(TypeName);
      switch (dt) {
        case RSExportPrimitiveType::DataTypeUnknown: {
          // User-defined types
          ET = RSExportRecordType::Create(Context,
                                          T->getAsStructureType(),
                                          TypeName);
          break;
        }
        case RSExportPrimitiveType::DataTypeRSMatrix2x2: {
          // 2 x 2 Matrix type
          ET = RSExportMatrixType::Create(Context,
                                          T->getAsStructureType(),
                                          TypeName,
                                          2);
          break;
        }
        case RSExportPrimitiveType::DataTypeRSMatrix3x3: {
          // 3 x 3 Matrix type
          ET = RSExportMatrixType::Create(Context,
                                          T->getAsStructureType(),
                                          TypeName,
                                          3);
          break;
        }
        case RSExportPrimitiveType::DataTypeRSMatrix4x4: {
          // 4 x 4 Matrix type
          ET = RSExportMatrixType::Create(Context,
                                          T->getAsStructureType(),
                                          TypeName,
                                          4);
          break;
        }
        default: {
          // Others are primitive types
          ET = RSExportPrimitiveType::Create(Context, T, TypeName);
          break;
        }
      }
      break;
    }
    case clang::Type::Builtin: {
      ET = RSExportPrimitiveType::Create(Context, T, TypeName);
      break;
    }
    case clang::Type::Pointer: {
      ET = RSExportPointerType::Create(Context,
               UNSAFE_CAST_TYPE(const clang::PointerType, T), TypeName);
      // FIXME: free the name (allocated in RSExportType::GetTypeName)
      delete [] TypeName.data();
      break;
    }
    case clang::Type::ExtVector: {
      ET = RSExportVectorType::Create(Context,
               UNSAFE_CAST_TYPE(const clang::ExtVectorType, T), TypeName);
      break;
    }
    case clang::Type::ConstantArray: {
      ET = RSExportConstantArrayType::Create(
              Context,
              UNSAFE_CAST_TYPE(const clang::ConstantArrayType, T));
      break;
    }
    default: {
      Context->ReportError("unknown type cannot be exported: '%0'")
          << T->getTypeClassName();
      break;
    }
  }

  return ET;
}

RSExportType *RSExportType::Create(RSContext *Context, const clang::Type *T) {
  llvm::StringRef TypeName;
  if (NormalizeType(T, TypeName, Context, NULL)) {
    return Create(Context, T, TypeName);
  } else {
    return NULL;
  }
}

RSExportType *RSExportType::CreateFromDecl(RSContext *Context,
                                           const clang::VarDecl *VD) {
  return RSExportType::Create(Context, GetTypeOfDecl(VD));
}

size_t RSExportType::GetTypeStoreSize(const RSExportType *ET) {
  return ET->getRSContext()->getDataLayout()->getTypeStoreSize(
      ET->getLLVMType());
}

size_t RSExportType::GetTypeAllocSize(const RSExportType *ET) {
  if (ET->getClass() == RSExportType::ExportClassRecord)
    return static_cast<const RSExportRecordType*>(ET)->getAllocSize();
  else
    return ET->getRSContext()->getDataLayout()->getTypeAllocSize(
        ET->getLLVMType());
}

RSExportType::RSExportType(RSContext *Context,
                           ExportClass Class,
                           const llvm::StringRef &Name)
    : RSExportable(Context, RSExportable::EX_TYPE),
      mClass(Class),
      // Make a copy on Name since memory stored @Name is either allocated in
      // ASTContext or allocated in GetTypeName which will be destroyed later.
      mName(Name.data(), Name.size()),
      mLLVMType(NULL) {
  // Don't cache the type whose name start with '<'. Those type failed to
  // get their name since constructing their name in GetTypeName() requiring
  // complicated work.
  if (!Name.startswith(DUMMY_RS_TYPE_NAME_PREFIX))
    // TODO(zonr): Need to check whether the insertion is successful or not.
    Context->insertExportType(llvm::StringRef(Name), this);
  return;
}

bool RSExportType::keep() {
  if (!RSExportable::keep())
    return false;
  // Invalidate converted LLVM type.
  mLLVMType = NULL;
  return true;
}

bool RSExportType::equals(const RSExportable *E) const {
  CHECK_PARENT_EQUALITY(RSExportable, E);
  return (static_cast<const RSExportType*>(E)->getClass() == getClass());
}

RSExportType::~RSExportType() {
}

/************************** RSExportPrimitiveType **************************/
llvm::ManagedStatic<RSExportPrimitiveType::RSSpecificTypeMapTy>
RSExportPrimitiveType::RSSpecificTypeMap;

llvm::Type *RSExportPrimitiveType::RSObjectLLVMType = NULL;

bool RSExportPrimitiveType::IsPrimitiveType(const clang::Type *T) {
  if ((T != NULL) && (T->getTypeClass() == clang::Type::Builtin))
    return true;
  else
    return false;
}

RSExportPrimitiveType::DataType
RSExportPrimitiveType::GetRSSpecificType(const llvm::StringRef &TypeName) {
  if (TypeName.empty())
    return DataTypeUnknown;

  if (RSSpecificTypeMap->empty()) {
    for (int i = 0; i < MatrixAndObjectDataTypesCount; i++) {
      RSSpecificTypeMap->GetOrCreateValue(MatrixAndObjectDataTypes[i].name,
                                          MatrixAndObjectDataTypes[i].dataType);
    }
  }

  RSSpecificTypeMapTy::const_iterator I = RSSpecificTypeMap->find(TypeName);
  if (I == RSSpecificTypeMap->end())
    return DataTypeUnknown;
  else
    return I->getValue();
}

RSExportPrimitiveType::DataType
RSExportPrimitiveType::GetRSSpecificType(const clang::Type *T) {
  T = GET_CANONICAL_TYPE(T);
  if ((T == NULL) || (T->getTypeClass() != clang::Type::Record))
    return DataTypeUnknown;

  return GetRSSpecificType( RSExportType::GetTypeName(T) );
}

bool RSExportPrimitiveType::IsRSMatrixType(DataType DT) {
    if (DT < 0 || DT >= DataTypeMax) {
        return false;
    }
    return gReflectionTypes[DT].category == MatrixDataType;
}

bool RSExportPrimitiveType::IsRSObjectType(DataType DT) {
    if (DT < 0 || DT >= DataTypeMax) {
        return false;
    }
    return gReflectionTypes[DT].category == ObjectDataType;
}

bool RSExportPrimitiveType::IsStructureTypeWithRSObject(const clang::Type *T) {
  bool RSObjectTypeSeen = false;
  while (T && T->isArrayType()) {
    T = T->getArrayElementTypeNoTypeQual();
  }

  const clang::RecordType *RT = T->getAsStructureType();
  if (!RT) {
    return false;
  }

  const clang::RecordDecl *RD = RT->getDecl();
  if (RD) {
    RD = RD->getDefinition();
  }
  if (!RD) {
    return false;
  }

  for (clang::RecordDecl::field_iterator FI = RD->field_begin(),
         FE = RD->field_end();
       FI != FE;
       FI++) {
    // We just look through all field declarations to see if we find a
    // declaration for an RS object type (or an array of one).
    const clang::FieldDecl *FD = *FI;
    const clang::Type *FT = RSExportType::GetTypeOfDecl(FD);
    while (FT && FT->isArrayType()) {
      FT = FT->getArrayElementTypeNoTypeQual();
    }

    RSExportPrimitiveType::DataType DT = GetRSSpecificType(FT);
    if (IsRSObjectType(DT)) {
      // RS object types definitely need to be zero-initialized
      RSObjectTypeSeen = true;
    } else {
      switch (DT) {
        case RSExportPrimitiveType::DataTypeRSMatrix2x2:
        case RSExportPrimitiveType::DataTypeRSMatrix3x3:
        case RSExportPrimitiveType::DataTypeRSMatrix4x4:
          // Matrix types should get zero-initialized as well
          RSObjectTypeSeen = true;
          break;
        default:
          // Ignore all other primitive types
          break;
      }
      while (FT && FT->isArrayType()) {
        FT = FT->getArrayElementTypeNoTypeQual();
      }
      if (FT->isStructureType()) {
        // Recursively handle structs of structs (even though these can't
        // be exported, it is possible for a user to have them internally).
        RSObjectTypeSeen |= IsStructureTypeWithRSObject(FT);
      }
    }
  }

  return RSObjectTypeSeen;
}

size_t RSExportPrimitiveType::GetSizeInBits(const RSExportPrimitiveType *EPT) {
  int type = EPT->getType();
  slangAssert((type > DataTypeUnknown && type < DataTypeMax) &&
              "RSExportPrimitiveType::GetSizeInBits : unknown data type");
  return gReflectionTypes[type].size_in_bits;
}

RSExportPrimitiveType::DataType
RSExportPrimitiveType::GetDataType(RSContext *Context, const clang::Type *T) {
  if (T == NULL)
    return DataTypeUnknown;

  switch (T->getTypeClass()) {
    case clang::Type::Builtin: {
      const clang::BuiltinType *BT =
        UNSAFE_CAST_TYPE(const clang::BuiltinType, T);
      BuiltinInfo *info = FindBuiltinType(BT->getKind());
      if (info != NULL) {
        return info->type;
      }
      // The size of type WChar depend on platform so we abandon the support
      // to them.
      Context->ReportError("built-in type cannot be exported: '%0'")
          << T->getTypeClassName();
      break;
    }
    case clang::Type::Record: {
      // must be RS object type
      return RSExportPrimitiveType::GetRSSpecificType(T);
    }
    default: {
      Context->ReportError("primitive type cannot be exported: '%0'")
          << T->getTypeClassName();
      break;
    }
  }

  return DataTypeUnknown;
}

RSExportPrimitiveType
*RSExportPrimitiveType::Create(RSContext *Context,
                               const clang::Type *T,
                               const llvm::StringRef &TypeName,
                               bool Normalized) {
  DataType DT = GetDataType(Context, T);

  if ((DT == DataTypeUnknown) || TypeName.empty())
    return NULL;
  else
    return new RSExportPrimitiveType(Context, ExportClassPrimitive, TypeName,
                                     DT, Normalized);
}

RSExportPrimitiveType *RSExportPrimitiveType::Create(RSContext *Context,
                                                     const clang::Type *T) {
  llvm::StringRef TypeName;
  if (RSExportType::NormalizeType(T, TypeName, Context, NULL)
      && IsPrimitiveType(T)) {
    return Create(Context, T, TypeName);
  } else {
    return NULL;
  }
}

llvm::Type *RSExportPrimitiveType::convertToLLVMType() const {
  llvm::LLVMContext &C = getRSContext()->getLLVMContext();

  if (isRSObjectType()) {
    // struct {
    //   int *p;
    // } __attribute__((packed, aligned(pointer_size)))
    //
    // which is
    //
    // <{ [1 x i32] }> in LLVM
    //
    if (RSObjectLLVMType == NULL) {
      std::vector<llvm::Type *> Elements;
      Elements.push_back(llvm::ArrayType::get(llvm::Type::getInt32Ty(C), 1));
      RSObjectLLVMType = llvm::StructType::get(C, Elements, true);
    }
    return RSObjectLLVMType;
  }

  switch (mType) {
    case DataTypeFloat32: {
      return llvm::Type::getFloatTy(C);
      break;
    }
    case DataTypeFloat64: {
      return llvm::Type::getDoubleTy(C);
      break;
    }
    case DataTypeBoolean: {
      return llvm::Type::getInt1Ty(C);
      break;
    }
    case DataTypeSigned8:
    case DataTypeUnsigned8: {
      return llvm::Type::getInt8Ty(C);
      break;
    }
    case DataTypeSigned16:
    case DataTypeUnsigned16:
    case DataTypeUnsigned565:
    case DataTypeUnsigned5551:
    case DataTypeUnsigned4444: {
      return llvm::Type::getInt16Ty(C);
      break;
    }
    case DataTypeSigned32:
    case DataTypeUnsigned32: {
      return llvm::Type::getInt32Ty(C);
      break;
    }
    case DataTypeSigned64:
    case DataTypeUnsigned64: {
      return llvm::Type::getInt64Ty(C);
      break;
    }
    default: {
      slangAssert(false && "Unknown data type");
    }
  }

  return NULL;
}

bool RSExportPrimitiveType::equals(const RSExportable *E) const {
  CHECK_PARENT_EQUALITY(RSExportType, E);
  return (static_cast<const RSExportPrimitiveType*>(E)->getType() == getType());
}

RSReflectionType *RSExportPrimitiveType::getRSReflectionType(DataType DT) {
  if (DT > DataTypeUnknown && DT < DataTypeMax) {
    return &gReflectionTypes[DT];
  } else {
    return NULL;
  }
}

/**************************** RSExportPointerType ****************************/

RSExportPointerType
*RSExportPointerType::Create(RSContext *Context,
                             const clang::PointerType *PT,
                             const llvm::StringRef &TypeName) {
  const clang::Type *PointeeType = GET_POINTEE_TYPE(PT);
  const RSExportType *PointeeET;

  if (PointeeType->getTypeClass() != clang::Type::Pointer) {
    PointeeET = RSExportType::Create(Context, PointeeType);
  } else {
    // Double or higher dimension of pointer, export as int*
    PointeeET = RSExportPrimitiveType::Create(Context,
                    Context->getASTContext().IntTy.getTypePtr());
  }

  if (PointeeET == NULL) {
    // Error diagnostic is emitted for corresponding pointee type
    return NULL;
  }

  return new RSExportPointerType(Context, TypeName, PointeeET);
}

llvm::Type *RSExportPointerType::convertToLLVMType() const {
  llvm::Type *PointeeType = mPointeeType->getLLVMType();
  return llvm::PointerType::getUnqual(PointeeType);
}

bool RSExportPointerType::keep() {
  if (!RSExportType::keep())
    return false;
  const_cast<RSExportType*>(mPointeeType)->keep();
  return true;
}

bool RSExportPointerType::equals(const RSExportable *E) const {
  CHECK_PARENT_EQUALITY(RSExportType, E);
  return (static_cast<const RSExportPointerType*>(E)
              ->getPointeeType()->equals(getPointeeType()));
}

/***************************** RSExportVectorType *****************************/
llvm::StringRef
RSExportVectorType::GetTypeName(const clang::ExtVectorType *EVT) {
  const clang::Type *ElementType = GET_EXT_VECTOR_ELEMENT_TYPE(EVT);
  llvm::StringRef name;

  if ((ElementType->getTypeClass() != clang::Type::Builtin))
    return name;

  const clang::BuiltinType *BT = UNSAFE_CAST_TYPE(const clang::BuiltinType,
                                                  ElementType);
  if ((EVT->getNumElements() < 1) ||
      (EVT->getNumElements() > 4))
    return name;

  BuiltinInfo *info = FindBuiltinType(BT->getKind());
  if (info != NULL) {
    // Compiler is smart enough to optimize following *big if branches* since
    // they all become "constant comparison" after macro expansion
    int I = EVT->getNumElements() - 1;
    if (I < kMaxVectorSize) {
      name = info->cname[I];
    } else {
      slangAssert(false && "Max vector is 4");
    }
  }
  return name;
}

RSExportVectorType *RSExportVectorType::Create(RSContext *Context,
                                               const clang::ExtVectorType *EVT,
                                               const llvm::StringRef &TypeName,
                                               bool Normalized) {
  slangAssert(EVT != NULL && EVT->getTypeClass() == clang::Type::ExtVector);

  const clang::Type *ElementType = GET_EXT_VECTOR_ELEMENT_TYPE(EVT);
  RSExportPrimitiveType::DataType DT =
      RSExportPrimitiveType::GetDataType(Context, ElementType);

  if (DT != RSExportPrimitiveType::DataTypeUnknown)
    return new RSExportVectorType(Context,
                                  TypeName,
                                  DT,
                                  Normalized,
                                  EVT->getNumElements());
  else
    return NULL;
}

llvm::Type *RSExportVectorType::convertToLLVMType() const {
  llvm::Type *ElementType = RSExportPrimitiveType::convertToLLVMType();
  return llvm::VectorType::get(ElementType, getNumElement());
}

bool RSExportVectorType::equals(const RSExportable *E) const {
  CHECK_PARENT_EQUALITY(RSExportPrimitiveType, E);
  return (static_cast<const RSExportVectorType*>(E)->getNumElement()
              == getNumElement());
}

/***************************** RSExportMatrixType *****************************/
RSExportMatrixType *RSExportMatrixType::Create(RSContext *Context,
                                               const clang::RecordType *RT,
                                               const llvm::StringRef &TypeName,
                                               unsigned Dim) {
  slangAssert((RT != NULL) && (RT->getTypeClass() == clang::Type::Record));
  slangAssert((Dim > 1) && "Invalid dimension of matrix");

  // Check whether the struct rs_matrix is in our expected form (but assume it's
  // correct if we're not sure whether it's correct or not)
  const clang::RecordDecl* RD = RT->getDecl();
  RD = RD->getDefinition();
  if (RD != NULL) {
    // Find definition, perform further examination
    if (RD->field_empty()) {
      Context->ReportError(
          RD->getLocation(),
          "invalid matrix struct: must have 1 field for saving values: '%0'")
          << RD->getName();
      return NULL;
    }

    clang::RecordDecl::field_iterator FIT = RD->field_begin();
    const clang::FieldDecl *FD = *FIT;
    const clang::Type *FT = RSExportType::GetTypeOfDecl(FD);
    if ((FT == NULL) || (FT->getTypeClass() != clang::Type::ConstantArray)) {
      Context->ReportError(RD->getLocation(),
                           "invalid matrix struct: first field should"
                           " be an array with constant size: '%0'")
          << RD->getName();
      return NULL;
    }
    const clang::ConstantArrayType *CAT =
      static_cast<const clang::ConstantArrayType *>(FT);
    const clang::Type *ElementType = GET_CONSTANT_ARRAY_ELEMENT_TYPE(CAT);
    if ((ElementType == NULL) ||
        (ElementType->getTypeClass() != clang::Type::Builtin) ||
        (static_cast<const clang::BuiltinType *>(ElementType)->getKind() !=
         clang::BuiltinType::Float)) {
      Context->ReportError(RD->getLocation(),
                           "invalid matrix struct: first field "
                           "should be a float array: '%0'")
          << RD->getName();
      return NULL;
    }

    if (CAT->getSize() != Dim * Dim) {
      Context->ReportError(RD->getLocation(),
                           "invalid matrix struct: first field "
                           "should be an array with size %0: '%1'")
          << (Dim * Dim) << (RD->getName());
      return NULL;
    }

    FIT++;
    if (FIT != RD->field_end()) {
      Context->ReportError(RD->getLocation(),
                           "invalid matrix struct: must have "
                           "exactly 1 field: '%0'")
          << RD->getName();
      return NULL;
    }
  }

  return new RSExportMatrixType(Context, TypeName, Dim);
}

llvm::Type *RSExportMatrixType::convertToLLVMType() const {
  // Construct LLVM type:
  // struct {
  //  float X[mDim * mDim];
  // }

  llvm::LLVMContext &C = getRSContext()->getLLVMContext();
  llvm::ArrayType *X = llvm::ArrayType::get(llvm::Type::getFloatTy(C),
                                            mDim * mDim);
  return llvm::StructType::get(C, X, false);
}

bool RSExportMatrixType::equals(const RSExportable *E) const {
  CHECK_PARENT_EQUALITY(RSExportType, E);
  return (static_cast<const RSExportMatrixType*>(E)->getDim() == getDim());
}

/************************* RSExportConstantArrayType *************************/
RSExportConstantArrayType
*RSExportConstantArrayType::Create(RSContext *Context,
                                   const clang::ConstantArrayType *CAT) {
  slangAssert(CAT != NULL && CAT->getTypeClass() == clang::Type::ConstantArray);

  slangAssert((CAT->getSize().getActiveBits() < 32) && "array too large");

  unsigned Size = static_cast<unsigned>(CAT->getSize().getZExtValue());
  slangAssert((Size > 0) && "Constant array should have size greater than 0");

  const clang::Type *ElementType = GET_CONSTANT_ARRAY_ELEMENT_TYPE(CAT);
  RSExportType *ElementET = RSExportType::Create(Context, ElementType);

  if (ElementET == NULL) {
    return NULL;
  }

  return new RSExportConstantArrayType(Context,
                                       ElementET,
                                       Size);
}

llvm::Type *RSExportConstantArrayType::convertToLLVMType() const {
  return llvm::ArrayType::get(mElementType->getLLVMType(), getSize());
}

bool RSExportConstantArrayType::keep() {
  if (!RSExportType::keep())
    return false;
  const_cast<RSExportType*>(mElementType)->keep();
  return true;
}

bool RSExportConstantArrayType::equals(const RSExportable *E) const {
  CHECK_PARENT_EQUALITY(RSExportType, E);
  const RSExportConstantArrayType *RHS =
      static_cast<const RSExportConstantArrayType*>(E);
  return ((getSize() == RHS->getSize()) &&
          (getElementType()->equals(RHS->getElementType())));
}

/**************************** RSExportRecordType ****************************/
RSExportRecordType *RSExportRecordType::Create(RSContext *Context,
                                               const clang::RecordType *RT,
                                               const llvm::StringRef &TypeName,
                                               bool mIsArtificial) {
  slangAssert(RT != NULL && RT->getTypeClass() == clang::Type::Record);

  const clang::RecordDecl *RD = RT->getDecl();
  slangAssert(RD->isStruct());

  RD = RD->getDefinition();
  if (RD == NULL) {
    slangAssert(false && "struct is not defined in this module");
    return NULL;
  }

  // Struct layout construct by clang. We rely on this for obtaining the
  // alloc size of a struct and offset of every field in that struct.
  const clang::ASTRecordLayout *RL =
      &Context->getASTContext().getASTRecordLayout(RD);
  slangAssert((RL != NULL) &&
      "Failed to retrieve the struct layout from Clang.");

  RSExportRecordType *ERT =
      new RSExportRecordType(Context,
                             TypeName,
                             RD->hasAttr<clang::PackedAttr>(),
                             mIsArtificial,
                             RL->getSize().getQuantity());
  unsigned int Index = 0;

  for (clang::RecordDecl::field_iterator FI = RD->field_begin(),
           FE = RD->field_end();
       FI != FE;
       FI++, Index++) {

    // FIXME: All fields should be primitive type
    slangAssert(FI->getKind() == clang::Decl::Field);
    clang::FieldDecl *FD = *FI;

    if (FD->isBitField()) {
      return NULL;
    }

    // Type
    RSExportType *ET = RSExportElement::CreateFromDecl(Context, FD);

    if (ET != NULL) {
      ERT->mFields.push_back(
          new Field(ET, FD->getName(), ERT,
                    static_cast<size_t>(RL->getFieldOffset(Index) >> 3)));
    } else {
      Context->ReportError(RD->getLocation(),
                           "field type cannot be exported: '%0.%1'")
          << RD->getName() << FD->getName();
      return NULL;
    }
  }

  return ERT;
}

llvm::Type *RSExportRecordType::convertToLLVMType() const {
  // Create an opaque type since struct may reference itself recursively.

  // TODO(sliao): LLVM took out the OpaqueType. Any other to migrate to?
  std::vector<llvm::Type*> FieldTypes;

  for (const_field_iterator FI = fields_begin(), FE = fields_end();
       FI != FE;
       FI++) {
    const Field *F = *FI;
    const RSExportType *FET = F->getType();

    FieldTypes.push_back(FET->getLLVMType());
  }

  llvm::StructType *ST = llvm::StructType::get(getRSContext()->getLLVMContext(),
                                               FieldTypes,
                                               mIsPacked);
  if (ST != NULL) {
    return ST;
  } else {
    return NULL;
  }
}

bool RSExportRecordType::keep() {
  if (!RSExportType::keep())
    return false;
  for (std::list<const Field*>::iterator I = mFields.begin(),
          E = mFields.end();
       I != E;
       I++) {
    const_cast<RSExportType*>((*I)->getType())->keep();
  }
  return true;
}

bool RSExportRecordType::equals(const RSExportable *E) const {
  CHECK_PARENT_EQUALITY(RSExportType, E);

  const RSExportRecordType *ERT = static_cast<const RSExportRecordType*>(E);

  if (ERT->getFields().size() != getFields().size())
    return false;

  const_field_iterator AI = fields_begin(), BI = ERT->fields_begin();

  for (unsigned i = 0, e = getFields().size(); i != e; i++) {
    if (!(*AI)->getType()->equals((*BI)->getType()))
      return false;
    AI++;
    BI++;
  }

  return true;
}

void RSExportType::convertToRTD(RSReflectionTypeData *rtd) const {
    memset(rtd, 0, sizeof(*rtd));
    rtd->vecSize = 1;

    switch(getClass()) {
    case RSExportType::ExportClassPrimitive: {
            const RSExportPrimitiveType *EPT = static_cast<const RSExportPrimitiveType*>(this);
            rtd->type = RSExportPrimitiveType::getRSReflectionType(EPT);
            return;
        }
    case RSExportType::ExportClassPointer: {
            const RSExportPointerType *EPT = static_cast<const RSExportPointerType*>(this);
            const RSExportType *PointeeType = EPT->getPointeeType();
            PointeeType->convertToRTD(rtd);
            rtd->isPointer = true;
            return;
        }
    case RSExportType::ExportClassVector: {
            const RSExportVectorType *EVT = static_cast<const RSExportVectorType*>(this);
            rtd->type = EVT->getRSReflectionType(EVT);
            rtd->vecSize = EVT->getNumElement();
            return;
        }
    case RSExportType::ExportClassMatrix: {
            const RSExportMatrixType *EMT = static_cast<const RSExportMatrixType*>(this);
            unsigned Dim = EMT->getDim();
            slangAssert((Dim >= 2) && (Dim <= 4));
            rtd->type = &gReflectionTypes[15 + Dim-2];
            return;
        }
    case RSExportType::ExportClassConstantArray: {
            const RSExportConstantArrayType* CAT =
              static_cast<const RSExportConstantArrayType*>(this);
            CAT->getElementType()->convertToRTD(rtd);
            rtd->arraySize = CAT->getSize();
            return;
        }
    case RSExportType::ExportClassRecord: {
            slangAssert(!"RSExportType::ExportClassRecord not implemented");
            return;// RS_TYPE_CLASS_NAME_PREFIX + ET->getName() + ".Item";
        }
    default: {
            slangAssert(false && "Unknown class of type");
        }
    }
}


}  // namespace slang
