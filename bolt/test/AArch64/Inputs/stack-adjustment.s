.text
.global test_aarch64_stack_adjustment
.type test_aarch64_stack_adjustment,@function

test_aarch64_stack_adjustment:
    # Direct stack pointer operations with arbitrary values
    sub     sp, sp, #64          // Allocate 64 bytes, adjust by +64
    add     sp, sp, #32          // Deallocate 32 bytes, adjust by -32
    sub     sp, sp, #0x100       // Allocate 256 bytes (hex value), adjust by +256
    
    # Test specific instructions mentioned for verification
    sub     sp, sp, #0x80        // Allocate 128 bytes, adjust by +128
    stp     x0, x1, [sp, #-0x10]! // Store pair pre-decrement, adjust by +16
    str     x2, [sp, #-0x10]!     // Store pre-decrement, adjust by +16
    ldr     x2, [sp], #0x10       // Load post-increment, adjust by -16
    ldp     x0, x1, [sp], #0x10   // Load pair post-increment, adjust by -16
    add     sp, sp, #0x80        // Deallocate 128 bytes, adjust by -128
    
    # Single register STR/LDR with arbitrary indices (pre/post)
    str     x0, [sp, #-128]!     // Store pre-decrement, adjust by +128
    str     w1, [sp, #-64]!      // Store pre-decrement (32-bit), adjust by +64
    ldr     x2, [sp, #96]        // Load post-increment, adjust by -96
    ldr     w3, [sp], #32        // Load post-increment (32-bit), adjust by -32
    
    # Register pair STP/LDP with arbitrary indices
    stp     x4, x5, [sp, #-256]! // Store pair pre-decrement, adjust by +256
    stp     w6, w7, [sp, #-128]! // Store pair pre-decrement (32-bit), adjust by +128
    ldp     x8, x9, [sp, #192]   // Load pair post-increment, adjust by -192
    ldp     w10, w11, [sp], #64  // Load pair post-increment (32-bit), adjust by -64
    
    # Mixed prologue/epilogue pattern (typical function frame setup/teardown)
    stp     x29, x30, [sp, #-80]!    // Save FP/LR, adjust by +80
    sub     sp, sp, #16             // Additional alignment padding, adjust by +16
    # Function body would be here...
    add     sp, sp, #16             // Remove alignment padding, adjust by -16
    ldp     x29, x30, [sp], #80     // Restore FP/LR, adjust by -80
    
    # Extended register operations with valid immediate ranges
    str     x12, [sp, #-256]!     // Large allocation, adjust by +256 (max valid)
    ldr     x13, [sp, #256]       // Large deallocation, adjust by -256 (max valid)
    stp     x14, x15, [sp, #-504]!  // Valid frame allocation, adjust by +504
    ldp     x16, x17, [sp], #504   // Valid restore, adjust by -504
    
    ret

.size   test_aarch64_stack_adjustment, .-test_aarch64_stack_adjustment

# Dummy entry point to make the test executable
.global _start
_start:
    bl test_aarch64_stack_adjustment
    b .