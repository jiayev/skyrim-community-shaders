#ifndef __NAMESPACED_CBUFFER_DEPENDENCY_HLSL__
#define __NAMESPACED_CBUFFER_DEPENDENCY_HLSL__

// FXC scopes the members of a cbuffer declared inside a namespace to that namespace. DXC puts them at
// global scope instead: NS::member no longer resolves, and names can collide with other globals (its
// FXC-compatibility mode, -Gec, is not available for SPIR-V). Members of such cbuffers are therefore
// declared as NSCB(NS, name); under DXC they get the namespace as a prefix, and NSCB_ALIAS /
// NSCB_ARRAY_ALIAS, placed in the namespace after the cbuffer, give them back their names there. FXC
// builds are unchanged.
#if defined(__hlsl_dx_compiler)
#	define NSCB(ns, name) ns##_##name
#	define NSCB_ALIAS(ns, type, name) static const type name = ns##_##name;
#	define NSCB_ARRAY_ALIAS(ns, type, name, count) static const type name[count] = ns##_##name;
#else
#	define NSCB(ns, name) name
#	define NSCB_ALIAS(ns, type, name)
#	define NSCB_ARRAY_ALIAS(ns, type, name, count)
#endif

#endif  // __NAMESPACED_CBUFFER_DEPENDENCY_HLSL__
