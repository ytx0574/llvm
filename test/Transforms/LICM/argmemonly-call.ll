; RUN: opt -S -basicaa -licm -licm-n2-threshold=0 %s | FileCheck %s
; RUN: opt -licm -basicaa -licm-n2-threshold=200 < %s -S | FileCheck %s --check-prefix=ALIAS-N2
; RUN: opt -aa-pipeline=basic-aa -licm-n2-threshold=0 -passes='require<aa>,require<targetir>,require<scalar-evolution>,require<opt-remark-emit>,loop(licm)' < %s -S | FileCheck %s
; RUN: opt -aa-pipeline=basic-aa -licm-n2-threshold=200 -passes='require<aa>,require<targetir>,require<scalar-evolution>,require<opt-remark-emit>,loop(licm)' < %s -S | FileCheck %s --check-prefix=ALIAS-N2

declare i32 @foo() readonly argmemonly nounwind
declare i32 @foo2() readonly nounwind
declare i32 @bar(i32* %loc2) readonly argmemonly nounwind

define void @test(i32* %loc) {
; CHECK-LABEL: @test
; CHECK: @foo
; CHECK-LABEL: loop:
  br label %loop

loop:
  %res = call i32 @foo()
  store i32 %res, i32* %loc
  br label %loop
}

; Negative test: show argmemonly is required
define void @test_neg(i32* %loc) {
; CHECK-LABEL: @test_neg
; CHECK-LABEL: loop:
; CHECK: @foo
  br label %loop

loop:
  %res = call i32 @foo2()
  store i32 %res, i32* %loc
  br label %loop
}

define void @test2(i32* noalias %loc, i32* noalias %loc2) {
; CHECK-LABEL: @test2
; CHECK: @bar
; CHECK-LABEL: loop:
  br label %loop

loop:
  %res = call i32 @bar(i32* %loc2)
  store i32 %res, i32* %loc
  br label %loop
}

; Negative test: %might clobber gep
define void @test3(i32* %loc) {
; CHECK-LABEL: @test3
; CHECK-LABEL: loop:
; CHECK: @bar
  br label %loop

loop:
  %res = call i32 @bar(i32* %loc)
  %gep = getelementptr i32, i32 *%loc, i64 1000000
  store i32 %res, i32* %gep
  br label %loop
}


; Negative test: %loc might alias %loc2
define void @test4(i32* %loc, i32* %loc2) {
; CHECK-LABEL: @test4
; CHECK-LABEL: loop:
; CHECK: @bar
  br label %loop

loop:
  %res = call i32 @bar(i32* %loc2)
  store i32 %res, i32* %loc
  br label %loop
}

declare i32 @foo_new(i32*) readonly
; With the default AST mechanism used by LICM for alias analysis,
; we clump foo_new with bar.
; With the N2 Alias analysis diagnostic tool, we are able to hoist the
; argmemonly bar call out of the loop.

define void @test5(i32* %loc2, i32* noalias %loc) {
; ALIAS-N2-LABEL: @test5
; ALIAS-N2: @bar
; ALIAS-N2-LABEL: loop:

; CHECK-LABEL: @test5
; CHECK-LABEL: loop:
; CHECK:  @bar
  br label %loop

loop:
  %res1 = call i32 @bar(i32* %loc2)
  %res = call i32 @foo_new(i32* %loc2)
  store volatile i32 %res1, i32* %loc
  br label %loop
}

