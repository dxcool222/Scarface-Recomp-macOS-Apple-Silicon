ps_1_4 
def     c1, 0.5, 0.5, 0.5, 0.5 
def     c2, 0, 0, 0, 0 
texld   r0,  t0 
mov    r1, c3 
dp3     r3,     r0, c0 
add     r3.xyz, r0, -r3 
+mad    r1.w,   r3.w, c5, r1_bx2 
mul_x2  r3.xyz, r3, c7 
mov r2, c6 
mad     r3.xyz, r2_bx2, r3.w, r3 
mov r1.b, r1.w
phase
mov r3.w, c2.w
add     r0, r3, r1.b 
add     r0.xyz, r0, r1.b 
