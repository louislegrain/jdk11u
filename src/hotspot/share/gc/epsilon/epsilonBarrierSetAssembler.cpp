#include "interpreter/interpreter.hpp"
#include "interpreter/interp_masm.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/thread.hpp"
#include "vmreg_x86.inline.hpp"
#include "precompiled.hpp"
#include "utilities/macros.hpp"
#include "gc/epsilon/epsilonBarrierSetAssembler.hpp"
#ifdef COMPILER1
#include "c1/c1_LIRAssembler.hpp"
#include "c1/c1_MacroAssembler.hpp"
#include "gc/shenandoah/c1/shenandoahBarrierSetC1.hpp"
#endif

#define __ masm->


void EpsilonBarrierSetAssembler::tlab_allocate(MacroAssembler* masm,
                                        Register thread, Register obj,
                                        Register var_size_in_bytes,
                                        int con_size_in_bytes,
                                        Register t1,
                                        Register t2,
                                        Label& slow_case) {
  

  assert_different_registers(obj, t1, t2);
  assert_different_registers(obj, var_size_in_bytes, t1);
  Register end = t2;
  if (!thread->is_valid()) {
#ifdef _LP64
    thread = r15_thread;
#else
    assert(t1->is_valid(), "need temp reg");
    thread = t1;
    __ get_thread(thread);
#endif
  }
  Label fast_case_end;
  Label slow_case_end;
  Label slow_case_end2;
  Label slow_case_and_print;

  
  __ jmp(slow_case);


  if (var_size_in_bytes == t2 &&  var_size_in_bytes != noreg) {
	__ push(var_size_in_bytes);
  }



  if (var_size_in_bytes == noreg) {
	__ mov64(t1, con_size_in_bytes);
  } else {
	__ movq(t1, var_size_in_bytes);   // movq  %rdi, %rcx 
  }

  //__ cmpq(t1, 65536);
  //__ jcc(Assembler::above, slow_case_end);

  __ shrq(t1, 3);				    // shrq  $3, %rcx 
  __ leaq(t2, Address(t1, -1));	    // leaq  -1(%rcx), %rax  
  __ bsrq(obj, t2);				    // bsrq  %rax, %rdx
  __ shlq(t2, 2);				    // salq  $2, %rax
  __ addq(obj, 29);                  // addq  $29, %rdx
  __ xorq(t2, obj);			        // xorq  %rdx, %rax
  __ cmpq(t1, 9);				    // cmpq  $9, %rcx 
  __ sbbq(t1, t1);				    // sbbq  %rcx, %rcx
  __ andq(t2, t1);                  // andq  %rcx, %rax
  //__ xorq(obj, t2);				    // xorq  %rdx, %rax
  __ xorq(t2, obj);				    // xorq  %rdx, %rax

  __ movq(t1, obj);
  //__ print_state();


  __ movq(t2, Address(thread, Thread::pseudo_tlab_offset()));
  //__ cmpq(t2, 0);
  //__ jcc(Assembler::equal, slow_case_end);
  //__ shlq(t1,3);
  //__ addq(t2, t1);
  __ movq(t2, Address(t2,t1, Address::times_8));
  //__ cmpq(t2, 0);
  //__ jcc(Assembler::equal, slow_case_end);
 

  __ xorq(obj, obj);
  __ movl(obj,Address(t2,0));
  __ cmpl(obj, Address(t2,4));
  __ jcc(Assembler::aboveEqual, slow_case_end);
  __ movq(t1, t2);
  __ addq(t1, 40);
  //__ shll(obj, 3);
  __ movq(obj, Address(t1, obj, Address::times_8)); //obj contient l'addresse finale
  __ movl(t1,Address(t2,0));
  __ addl(t1, 1);
  __ movl(Address(t2,0), t1);

  //__ warn("before 0 equal");
  //__ cmpq(obj, 0);
  //__ jcc(Assembler::equal, slow_case_end);


  if (var_size_in_bytes == t2 && var_size_in_bytes != noreg) {
	__ pop(var_size_in_bytes);
  }
  //__ warn("fast");
  __ jmp(fast_case_end);



  __ bind(slow_case_end);
  //__ warn("slow");
  if (var_size_in_bytes == t2 && var_size_in_bytes != noreg) {
	__ pop(var_size_in_bytes);
  }
  __ jmp(slow_case);



  __ bind(fast_case_end);
  //__ print_state();
 

}
