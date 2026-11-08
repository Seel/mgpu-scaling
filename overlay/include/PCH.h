#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#pragma warning(push)
#include "F4SE/F4SE.h"
#include "RE/Fallout.h"
#include "REX/FModule.h"
#include "REX/REX/Singleton.h"
#pragma warning(pop)

#include "Windows.h"

#undef DEBUG
#undef ERROR

#include <cstdint>
#include <string>
#include <typeinfo>
using namespace std::literals;

#include "detours/detours.h"

#include "SimpleMath.h"

using float2 = DirectX::SimpleMath::Vector2;
using float3 = DirectX::SimpleMath::Vector3;
using float4 = DirectX::SimpleMath::Vector4;
using float4x4 = DirectX::SimpleMath::Matrix;
using uint = uint32_t;

#include <directx/d3dx12.h>

#include <magic_enum/magic_enum.hpp>

#define DLLEXPORT __declspec(dllexport)

namespace stl
{
	template <class T>
	void write_thunk_call(std::uintptr_t a_src)
	{
		auto& trampoline = REL::GetTrampoline();
		T::func = trampoline.write_call<5>(a_src, T::thunk);
	}

	template <class F, size_t index, class T>
	void write_vfunc()
	{
		REL::Relocation<std::uintptr_t> vtbl{ F::VTABLE[index] };
		T::func = vtbl.write_vfunc(T::size, T::thunk);
	}

	template <class F, class T>
	void write_vfunc()
	{
		write_vfunc<F, 0, T>();
	}

	template <std::size_t idx, class T>
	void write_vfunc(REL::ID id)
	{
		REL::Relocation<std::uintptr_t> vtbl{ id };
		T::func = vtbl.write_vfunc(idx, T::thunk);
	}

	namespace detail
	{
		// MGPU Scaling: where a function's entry jumps when another plugin hooked it first, or 0.
		// Microsoft Detours (Community Shaders' frame annotations) and CommonLib's 5-byte branches
		// write "jmp rel32"; 6-byte branches write "jmp [rip+disp32]".
		inline std::uintptr_t HookedEntryTarget(std::uintptr_t a_entry) noexcept
		{
			const auto* code = reinterpret_cast<const std::uint8_t*>(a_entry);
			if (code[0] == 0xE9)
				return a_entry + 5 + static_cast<std::intptr_t>(*reinterpret_cast<const std::int32_t*>(code + 1));
			if (code[0] == 0xFF && code[1] == 0x25) {
				const auto slot = a_entry + 6 + static_cast<std::intptr_t>(*reinterpret_cast<const std::int32_t*>(code + 2));
				return *reinterpret_cast<const std::uintptr_t*>(slot);
			}
			return 0;
		}

		// Nukem's DetourFunction copies the entry's first instructions into its trampoline without
		// fixing relative ones. On a function another plugin hooked first, that copied jump lands on
		// garbage and the game crashes on the first call (Community Shaders with frame annotations:
		// DrawWorld::Forward and DrawWorld::Imagespace, on the first world frame). Chain behind the
		// other hook instead: the entry jumps here, and the original is where its jump went, which
		// runs the other hook and then the game's function. Returns the original.
		inline std::uintptr_t DetourOrChain(std::uintptr_t a_target, std::uintptr_t a_thunk, const char* a_name)
		{
			if (const auto previous = HookedEntryTarget(a_target)) {
				REL::GetTrampoline().write_jmp<5>(a_target, a_thunk);
				REX::INFO("[HOOK] {} at {:#x} was already hooked by another plugin (jump to {:#x}); chained behind it",
					a_name, a_target, previous);
				return previous;
			}
			return Detours::X64::DetourFunction(a_target, a_thunk);
		}
	}

	template <class T>
	void detour_thunk(REL::ID a_relId)
	{
		*(uintptr_t*)&T::func = detail::DetourOrChain(a_relId.address(), (uintptr_t)&T::thunk, typeid(T).name());
	}

	template <class T>
	void detour_thunk_ignore_func(REL::ID a_relId)
	{
		std::ignore = detail::DetourOrChain(a_relId.address(), (uintptr_t)&T::thunk, typeid(T).name());
	}

	template <std::size_t idx, class T>
	void detour_vfunc(void* target)
	{
		*(uintptr_t*)&T::func = Detours::X64::DetourClassVTable(*(uintptr_t*)target, &T::thunk, idx);
	}
}


namespace DX
{
	// Helper class for COM exceptions
	class com_exception : public std::exception
	{
	public:
		explicit com_exception(HRESULT hr) noexcept :
			result(hr) {}

		const char* what() const override
		{
			static char s_str[64] = {};
			sprintf_s(s_str, "Failure with HRESULT of %08X", static_cast<unsigned int>(result));
			return s_str;
		}

	private:
		HRESULT result;
	};

	// Helper utility converts D3D API failures into exceptions.
	inline void ThrowIfFailed(HRESULT hr)
	{
		if (FAILED(hr)) {
			throw com_exception(hr);
		}
	}
}

#include "Plugin.h"
