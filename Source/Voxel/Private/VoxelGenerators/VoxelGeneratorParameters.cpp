// Copyright 2021 Phyronnaz

#include "VoxelGenerators/VoxelGeneratorParameters.h"
#include "UObject/Package.h"

FString FVoxelGeneratorParameterTerminalType::ToString_Terminal() const
{
	switch (PropertyType)
	{
	default: ensure(false);
	case EVoxelGeneratorParameterPropertyType::Float: return TEXT("float");
	case EVoxelGeneratorParameterPropertyType::Int: return TEXT("int");
	case EVoxelGeneratorParameterPropertyType::Bool: return TEXT("bool");
	case EVoxelGeneratorParameterPropertyType::Name: return TEXT("name");
	case EVoxelGeneratorParameterPropertyType::Object: return FString::Printf(TEXT("%s (object)"), *PropertyClass.ToString());
	case EVoxelGeneratorParameterPropertyType::Struct: return FString::Printf(TEXT("%s (struct)"), *PropertyClass.ToString());
	}
}

bool FVoxelGeneratorParameterTerminalType::CanBeAssignedFrom_Terminal(const FVoxelGeneratorParameterTerminalType& Other) const
{
	switch (PropertyType)
	{
	default: ensure(false);
	case EVoxelGeneratorParameterPropertyType::Float:
	{
		switch (Other.PropertyType)
		{
		case EVoxelGeneratorParameterPropertyType::Float: return true;
		case EVoxelGeneratorParameterPropertyType::Int: return true;
		default: return false;
		}
	}
	case EVoxelGeneratorParameterPropertyType::Int:
	{
		switch (Other.PropertyType)
		{
		case EVoxelGeneratorParameterPropertyType::Int: return true;
		default: return false;
		}
	}
	case EVoxelGeneratorParameterPropertyType::Bool:
	{
		switch (Other.PropertyType)
		{
		case EVoxelGeneratorParameterPropertyType::Bool: return true;
		default: return false;
		}
	}
	case EVoxelGeneratorParameterPropertyType::Name:
	{
		switch (Other.PropertyType)
		{
		case EVoxelGeneratorParameterPropertyType::Name: return true;
		default: return false;
		}
	}
	case EVoxelGeneratorParameterPropertyType::Object:
	{
		switch (Other.PropertyType)
		{
		case EVoxelGeneratorParameterPropertyType::Object:
		{
			auto* ThisClass = FindObject<UClass>(PropertyClassPackage, *PropertyClass.ToString());
			auto* OtherClass = FindObject<UClass>(PropertyClassPackage, *Other.PropertyClass.ToString());

			if (!ThisClass || !OtherClass)
			{
				ensureVoxelSlow(false);
				return false;
			}

			return OtherClass->IsChildOf(ThisClass);
		}
		default: return false;
		}
	}
	case EVoxelGeneratorParameterPropertyType::Struct:
	{
		switch (Other.PropertyType)
		{
		case EVoxelGeneratorParameterPropertyType::Struct:
		{
			auto* ThisStruct = FindObject<UScriptStruct>(PropertyClassPackage, *PropertyClass.ToString());
			auto* OtherStruct = FindObject<UScriptStruct>(PropertyClassPackage, *Other.PropertyClass.ToString());

			if (!ThisStruct || !OtherStruct)
			{
				ensureVoxelSlow(false);
				return false;
			}

			return OtherStruct->IsChildOf(ThisStruct);
		}
		default: return false;
		}
	}
	}
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

FVoxelGeneratorParameterType::FVoxelGeneratorParameterType(FProperty& Property)
{
	if (Property.IsA<FFloatProperty>())
	{
		PropertyType = EVoxelGeneratorParameterPropertyType::Float;
		PropertyClassPackage = nullptr;
	}
	else if (Property.IsA<FIntProperty>())
	{
		PropertyType = EVoxelGeneratorParameterPropertyType::Int;
		PropertyClassPackage = nullptr;
	}
	else if (Property.IsA<FBoolProperty>())
	{
		PropertyType = EVoxelGeneratorParameterPropertyType::Bool;
		PropertyClassPackage = nullptr;
	}
	else if (Property.IsA<FNameProperty>())
	{
		PropertyType = EVoxelGeneratorParameterPropertyType::Name;
		PropertyClassPackage = nullptr;
	}
	else if (Property.IsA<FObjectProperty>())
	{
		PropertyType = EVoxelGeneratorParameterPropertyType::Object;

		auto* ObjectProperty = CastField<FObjectProperty>(&Property);
		PropertyClass = ObjectProperty->PropertyClass->GetFName();
		PropertyClassPackage = ObjectProperty->PropertyClass->GetPackage();
	}
	else if (Property.IsA<FSoftObjectProperty>())
	{
		PropertyType = EVoxelGeneratorParameterPropertyType::Object;

		auto* ObjectProperty = CastField<FSoftObjectProperty>(&Property);
		PropertyClass = ObjectProperty->PropertyClass->GetFName();
		PropertyClassPackage = ObjectProperty->PropertyClass->GetPackage();
	}
	else if (Property.IsA<FStructProperty>())
	{
		PropertyType = EVoxelGeneratorParameterPropertyType::Struct;

		auto* ObjectProperty = CastField<FStructProperty>(&Property);
		PropertyClass = ObjectProperty->Struct->GetFName();
		PropertyClassPackage = ObjectProperty->Struct->GetPackage();
	}
	else if (Property.IsA<FArrayProperty>())
	{
		auto* ArrayProperty = CastField<FArrayProperty>(&Property);
		
		ContainerType = EVoxelGeneratorParameterContainerType::Array;
		
		const auto InnerType = FVoxelGeneratorParameterType(*ArrayProperty->Inner);
		ensure(InnerType.ContainerType == EVoxelGeneratorParameterContainerType::None);
		
		PropertyType = InnerType.PropertyType;
		PropertyClass = InnerType.PropertyClass;
		PropertyClassPackage = InnerType.PropertyClassPackage;
	}
	else if (Property.IsA<FSetProperty>())
	{
		auto* SetProperty = CastField<FSetProperty>(&Property);
		
		ContainerType = EVoxelGeneratorParameterContainerType::Set;
		
		const auto InnerType = FVoxelGeneratorParameterType(*SetProperty->ElementProp);
		ensure(InnerType.ContainerType == EVoxelGeneratorParameterContainerType::None);
		
		PropertyType = InnerType.PropertyType;
		PropertyClass = InnerType.PropertyClass;
		PropertyClassPackage = InnerType.PropertyClassPackage;
	}
	else if (Property.IsA<FMapProperty>())
	{
		auto* MapProperty = CastField<FMapProperty>(&Property);
		
		ContainerType = EVoxelGeneratorParameterContainerType::Map;
		
		const auto KeyType = FVoxelGeneratorParameterType(*MapProperty->KeyProp);
		ensure(KeyType.ContainerType == EVoxelGeneratorParameterContainerType::None);
		
		const auto LocalValueType = FVoxelGeneratorParameterType(*MapProperty->ValueProp);
		ensure(LocalValueType.ContainerType == EVoxelGeneratorParameterContainerType::None);
		
		PropertyType = KeyType.PropertyType;
		PropertyClass = KeyType.PropertyClass;
		PropertyClassPackage = KeyType.PropertyClassPackage;

		ValueType = FVoxelGeneratorParameterTerminalType(LocalValueType);
	}
	else
	{
		ensureMsgf(false, TEXT("Property: %s"), *Property.GetNameCPP());
	}
}

FString FVoxelGeneratorParameterType::ToString() const
{
	switch (ContainerType)
	{
	default: ensure(false);
	case EVoxelGeneratorParameterContainerType::None: return ToString_Terminal();
	case EVoxelGeneratorParameterContainerType::Array: return "Array of " + ToString_Terminal();
	case EVoxelGeneratorParameterContainerType::Set: return "Set of " + ToString_Terminal();
	case EVoxelGeneratorParameterContainerType::Map: return "Map of " + ToString_Terminal() + " to " + ValueType.ToString_Terminal();
	}
}

bool FVoxelGeneratorParameterType::CanBeAssignedFrom(const FVoxelGeneratorParameterType& Other) const
{
	if (ContainerType != Other.ContainerType)
	{
		return false;
	}

	if (ContainerType == EVoxelGeneratorParameterContainerType::Map && !ValueType.CanBeAssignedFrom_Terminal(Other.ValueType))
	{
		return false;
	}

	return CanBeAssignedFrom_Terminal(Other);
}