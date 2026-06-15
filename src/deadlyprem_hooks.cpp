// deadlyprem - ReXGlue Recompiled Project
//
// Implementations for [functions] (named call targets) and [[midasm_hook]]
// entries declared in deadlyprem_config.toml. Codegen emits the declarations
// from this file's symbol names; the bodies live here.
//
// Conventions:
//   void MyNamedFunction(PPCContext& ctx, uint8_t* base);   // [functions]
//   void MyMidasmHook();                                    // bare hook
//   void MyMidasmHook(PPCRegister& rN);                     // with `registers = ["rN"]`
//   bool MyMidasmHook(PPCRegister& rN);                     // for `jump_address_on_true`

#include "generated/default/deadlyprem_init.h"

// Hooks land here as the codegen learns about specific guest crashes.
