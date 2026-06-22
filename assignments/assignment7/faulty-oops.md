# Kernel Oops Analysis: faulty_write

When running the command `echo "hello_world" > /dev/faulty` from the command line of the running QEMU image, the kernel experiences an oops (a kernel panic/fault) and prints a stack trace to the console.

## The Cause

The `faulty` driver is intentionally designed to produce this fault. When we write to `/dev/faulty`, the Virtual File System (VFS) routes this request to the `faulty_write` function defined in the module.

Looking at `faulty.c`:

```c
ssize_t faulty_write (struct file *filp, const char __user *buf, size_t count,
		loff_t *pos)
{
	/* make a simple fault by dereferencing a NULL pointer */
	*(int *)0 = 0;
	return 0;
}
```

The line `*(int *)0 = 0;` attempts to dereference a NULL pointer by casting `0` to an integer pointer and then assigning `0` to that address. The virtual address `0` is an invalid, unmapped memory location.

## Kernel Oops Output

Because the kernel accesses an invalid virtual address, the MMU generates a page fault. The kernel's page fault handler checks the address and determines that it's a kernel-space access to an invalid address, which is illegal. 

This results in a kernel oops with a message similar to:
`Unable to handle kernel NULL pointer dereference at virtual address 0000000000000000`

The oops output includes:
1. **The faulting address**: `0000000000000000`
2. **The Program Counter (PC)**: Points inside the `faulty_write` function in the `faulty` module.
3. **The Call Trace**: Shows the path the kernel took to get there, typically starting from the system call handler (`ksys_write` or `sys_write`), transitioning through the VFS (`vfs_write`), and finally calling the driver's write operation (`faulty_write`).
4. **Register dump**: The state of the CPU registers at the time of the fault.

## Summary

The driver deliberately attempts a write to a NULL pointer, causing an invalid memory access. The kernel intercepts this hardware page fault, recognizes the illegal access in kernel mode, and outputs an oops to the log before terminating the offending process (in this case, the `echo` command).
