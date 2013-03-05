/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1995, 1996, 1997, 2000, 2001, 05 by Ralf Baechle
 * Copyright (C) 1999, 2000 Silicon Graphics, Inc.
 * Copyright (C) 2001 MIPS Technologies, Inc.
 */
#include <linux/capability.h>
#include <linux/errno.h>
#include <linux/linkage.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/smp.h>
#include <linux/mman.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/syscalls.h>
#include <linux/file.h>
#include <linux/utsname.h>
#include <linux/unistd.h>
#include <linux/sem.h>
#include <linux/msg.h>
#include <linux/shm.h>
#include <linux/compiler.h>
#include <linux/module.h>
#include <linux/ipc.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/elf.h>

#include <asm/asm.h>
#include <asm/branch.h>
#include <asm/cachectl.h>
#include <asm/cacheflush.h>
#include <asm/asm-offsets.h>
#include <asm/signal.h>
#include <asm/sim.h>
#include <asm/shmparam.h>
#include <asm/sysmips.h>
#include <asm/uaccess.h>

/*
 * For historic reasons the pipe(2) syscall on MIPS has an unusual calling
 * convention.  It returns results in registers $v0 / $v1 which means there
 * is no need for it to do verify the validity of a userspace pointer
 * argument.  Historically that used to be expensive in Linux.  These days
 * the performance advantage is negligible.
 */
asmlinkage int sysm_pipe(nabi_no_regargs volatile struct pt_regs regs)
{
	int fd[2];
	int error, res;

	error = do_pipe_flags(fd, 0);
	if (error) {
		res = error;
		goto out;
	}
	regs.regs[3] = fd[1];
	res = fd[0];
out:
	return res;
}

unsigned long shm_align_mask = PAGE_SIZE - 1;	/* Sane caches */

EXPORT_SYMBOL(shm_align_mask);

#define COLOUR_ALIGN(addr,pgoff)				\
	((((addr) + shm_align_mask) & ~shm_align_mask) +	\
	 (((pgoff) << PAGE_SHIFT) & shm_align_mask))

unsigned long arch_get_unmapped_area(struct file *filp, unsigned long addr,
	unsigned long len, unsigned long pgoff, unsigned long flags)
{
	struct vm_area_struct * vmm;
	int do_color_align;
	unsigned long task_size;

#ifdef CONFIG_32BIT
	task_size = TASK_SIZE;
#else /* Must be CONFIG_64BIT*/
	task_size = test_thread_flag(TIF_32BIT_ADDR) ? TASK_SIZE32 : TASK_SIZE;
#endif

	if (len > task_size)
		return -ENOMEM;

	if (flags & MAP_FIXED) {
		/* Even MAP_FIXED mappings must reside within task_size.  */
		if (task_size - len < addr)
			return -EINVAL;

		/*
		 * We do not accept a shared mapping if it would violate
		 * cache aliasing constraints.
		 */
		if ((flags & MAP_SHARED) &&
		    ((addr - (pgoff << PAGE_SHIFT)) & shm_align_mask))
			return -EINVAL;
		return addr;
	}

	do_color_align = 0;
	if (filp || (flags & MAP_SHARED))
		do_color_align = 1;
	if (addr) {
		if (do_color_align)
			addr = COLOUR_ALIGN(addr, pgoff);
		else
			addr = PAGE_ALIGN(addr);
		vmm = find_vma(current->mm, addr);
		if (task_size - len >= addr &&
		    (!vmm || addr + len <= vmm->vm_start))
			return addr;
	}
	addr = current->mm->mmap_base;
	if (do_color_align)
		addr = COLOUR_ALIGN(addr, pgoff);
	else
		addr = PAGE_ALIGN(addr);

	for (vmm = find_vma(current->mm, addr); ; vmm = vmm->vm_next) {
		/* At this point:  (!vmm || addr < vmm->vm_end). */
		if (task_size - len < addr)
			return -ENOMEM;
		if (!vmm || addr + len <= vmm->vm_start)
			return addr;
		addr = vmm->vm_end;
		if (do_color_align)
			addr = COLOUR_ALIGN(addr, pgoff);
	}
}

void arch_pick_mmap_layout(struct mm_struct *mm)
{
	unsigned long random_factor = 0UL;

	if (current->flags & PF_RANDOMIZE) {
		random_factor = get_random_int();
		random_factor = random_factor << PAGE_SHIFT;
		if (TASK_IS_32BIT_ADDR)
			random_factor &= 0xfffffful;
		else
			random_factor &= 0xffffffful;
	}

	mm->mmap_base = TASK_UNMAPPED_BASE + random_factor;
	mm->get_unmapped_area = arch_get_unmapped_area;
	mm->unmap_area = arch_unmap_area;
}

static inline unsigned long brk_rnd(void)
{
	unsigned long rnd = get_random_int();

	rnd = rnd << PAGE_SHIFT;
	/* 8MB for 32bit, 256MB for 64bit */
	if (TASK_IS_32BIT_ADDR)
		rnd = rnd & 0x7ffffful;
	else
		rnd = rnd & 0xffffffful;

	return rnd;
}

unsigned long arch_randomize_brk(struct mm_struct *mm)
{
	unsigned long base = mm->brk;
	unsigned long ret;

	ret = PAGE_ALIGN(base + brk_rnd());

	if (ret < mm->brk)
		return mm->brk;

	return ret;
}

SYSCALL_DEFINE6(mips_mmap, unsigned long, addr, unsigned long, len,
	unsigned long, prot, unsigned long, flags, unsigned long,
	fd, off_t, offset)
{
	unsigned long result;

	result = -EINVAL;
	if (offset & ~PAGE_MASK)
		goto out;

	result = sys_mmap_pgoff(addr, len, prot, flags, fd, offset >> PAGE_SHIFT);

out:
	return result;
}

SYSCALL_DEFINE6(mips_mmap2, unsigned long, addr, unsigned long, len,
	unsigned long, prot, unsigned long, flags, unsigned long, fd,
	unsigned long, pgoff)
{
	if (pgoff & (~PAGE_MASK >> 12))
		return -EINVAL;

	return sys_mmap_pgoff(addr, len, prot, flags, fd, pgoff >> (PAGE_SHIFT-12));
}

save_static_function(sys_fork);
static int __used noinline
_sys_fork(nabi_no_regargs struct pt_regs regs)
{
	return do_fork(SIGCHLD, regs.regs[29], &regs, 0, NULL, NULL);
}

save_static_function(sys_clone);
static int __used noinline
_sys_clone(nabi_no_regargs struct pt_regs regs)
{
	unsigned long clone_flags;
	unsigned long newsp;
	int __user *parent_tidptr, *child_tidptr;

	clone_flags = regs.regs[4];
	newsp = regs.regs[5];
	if (!newsp)
		newsp = regs.regs[29];
	parent_tidptr = (int __user *) regs.regs[6];
#ifdef CONFIG_32BIT
	/* We need to fetch the fifth argument off the stack.  */
	child_tidptr = NULL;
	if (clone_flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) {
		int __user *__user *usp = (int __user *__user *) regs.regs[29];
		if (regs.regs[2] == __NR_syscall) {
			if (get_user (child_tidptr, &usp[5]))
				return -EFAULT;
		}
		else if (get_user (child_tidptr, &usp[4]))
			return -EFAULT;
	}
#else
	child_tidptr = (int __user *) regs.regs[8];
#endif
	return do_fork(clone_flags, newsp, &regs, 0,
	               parent_tidptr, child_tidptr);
}

/*
 * sys_execve() executes a new program.
 */
asmlinkage int sys_execve(nabi_no_regargs struct pt_regs regs)
{
	int error;
	char * filename;

	filename = getname((const char __user *) (long)regs.regs[4]);
	error = PTR_ERR(filename);
	if (IS_ERR(filename))
		goto out;
	error = do_execve(filename,
			  (const char __user *const __user *) (long)regs.regs[5],
	                  (const char __user *const __user *) (long)regs.regs[6],
			  &regs);
	putname(filename);

out:
	return error;
}

SYSCALL_DEFINE1(set_thread_area, unsigned long, addr)
{
	struct thread_info *ti = task_thread_info(current);

	ti->tp_value = addr;
	if (cpu_has_userlocal)
		write_c0_userlocal(addr);

	return 0;
}

static inline int mips_atomic_set(struct pt_regs *regs,
	unsigned long addr, unsigned long new)
{
	unsigned long old, tmp;
	unsigned int err;

	if (unlikely(addr & 3))
		return -EINVAL;

	if (unlikely(!access_ok(VERIFY_WRITE, addr, 4)))
		return -EINVAL;

	if (cpu_has_llsc && R10000_LLSC_WAR) {
		__asm__ __volatile__ (
		"	.set	mips3					\n"
		"	li	%[err], 0				\n"
		"1:	ll	%[old], (%[addr])			\n"
		"	move	%[tmp], %[new]				\n"
		"2:	sc	%[tmp], (%[addr])			\n"
		"	beqzl	%[tmp], 1b				\n"
		"3:							\n"
		"	.section .fixup,\"ax\"				\n"
		"4:	li	%[err], %[efault]			\n"
		"	j	3b					\n"
		"	.previous					\n"
		"	.section __ex_table,\"a\"			\n"
		"	"STR(PTR)"	1b, 4b				\n"
		"	"STR(PTR)"	2b, 4b				\n"
		"	.previous					\n"
		"	.set	mips0					\n"
		: [old] "=&r" (old),
		  [err] "=&r" (err),
		  [tmp] "=&r" (tmp)
		: [addr] "r" (addr),
		  [new] "r" (new),
		  [efault] "i" (-EFAULT)
		: "memory");
	} else if (cpu_has_llsc) {
		__asm__ __volatile__ (
		"	.set	mips3					\n"
		"	li	%[err], 0				\n"
		"1:	ll	%[old], (%[addr])			\n"
		"	move	%[tmp], %[new]				\n"
		"2:	sc	%[tmp], (%[addr])			\n"
		"	bnez	%[tmp], 4f				\n"
		"3:							\n"
		"	.subsection 2					\n"
		"4:	b	1b					\n"
		"	.previous					\n"
		"							\n"
		"	.section .fixup,\"ax\"				\n"
		"5:	li	%[err], %[efault]			\n"
		"	j	3b					\n"
		"	.previous					\n"
		"	.section __ex_table,\"a\"			\n"
		"	"STR(PTR)"	1b, 5b				\n"
		"	"STR(PTR)"	2b, 5b				\n"
		"	.previous					\n"
		"	.set	mips0					\n"
		: [old] "=&r" (old),
		  [err] "=&r" (err),
		  [tmp] "=&r" (tmp)
		: [addr] "r" (addr),
		  [new] "r" (new),
		  [efault] "i" (-EFAULT)
		: "memory");
	} else {
		do {
			preempt_disable();
			ll_bit = 1;
			ll_task = current;
			preempt_enable();

			err = __get_user(old, (unsigned int *) addr);
			err |= __put_user(new, (unsigned int *) addr);
			if (err)
				break;
			rmb();
		} while (!ll_bit);
	}

	if (unlikely(err))
		return err;

	regs->regs[2] = old;
	regs->regs[7] = 0;	/* No error */

	/*
	 * Don't let your children do this ...
	 */
	__asm__ __volatile__(
	"	move	$29, %0						\n"
	"	j	syscall_exit					\n"
	: /* no outputs */
	: "r" (regs));

	/* unreached.  Honestly.  */
	while (1);
}

save_static_function(sys_sysmips);
static int __used noinline
_sys_sysmips(nabi_no_regargs struct pt_regs regs)
{
	long cmd, arg1, arg2, arg3;

	cmd = regs.regs[4];
	arg1 = regs.regs[5];
	arg2 = regs.regs[6];
	arg3 = regs.regs[7];

	switch (cmd) {
	case MIPS_ATOMIC_SET:
		return mips_atomic_set(&regs, arg1, arg2);

	case MIPS_FIXADE:
		if (arg1 & ~3)
			return -EINVAL;

		if (arg1 & 1)
			set_thread_flag(TIF_FIXADE);
		else
			clear_thread_flag(TIF_FIXADE);
		if (arg1 & 2)
			set_thread_flag(TIF_LOGADE);
		else
			clear_thread_flag(TIF_FIXADE);

		return 0;

	case FLUSH_CACHE:
		__flush_cache_all();
		return 0;
	}

	return -EINVAL;
}

/*
 * No implemented yet ...
 */
SYSCALL_DEFINE3(cachectl, char *, addr, int, nbytes, int, op)
{
	return -ENOSYS;
}

/*
 * If we ever come here the user sp is bad.  Zap the process right away.
 * Due to the bad stack signaling wouldn't work.
 */
asmlinkage void bad_stack(void)
{
	do_exit(SIGSEGV);
}

/*
 * Do a system call from kernel instead of calling sys_execve so we
 * end up with proper pt_regs.
 */
int kernel_execve(const char *filename,
		  const char *const argv[],
		  const char *const envp[])
{
	register unsigned long __a0 asm("$4") = (unsigned long) filename;
	register unsigned long __a1 asm("$5") = (unsigned long) argv;
	register unsigned long __a2 asm("$6") = (unsigned long) envp;
	register unsigned long __a3 asm("$7");
	unsigned long __v0;

	__asm__ volatile ("					\n"
	"	.set	noreorder				\n"
	"	li	$2, %5		# __NR_execve		\n"
	"	syscall						\n"
	"	move	%0, $2					\n"
	"	.set	reorder					\n"
	: "=&r" (__v0), "=r" (__a3)
	: "r" (__a0), "r" (__a1), "r" (__a2), "i" (__NR_execve)
	: "$2", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24",
	  "memory");

	if (__a3 == 0)
		return __v0;

	return -__v0;
}

#ifdef CONFIG_CPU_SUPPORTS_VECTOR_COPROCESSOR
/*
 * this function will remap the addr from the user space to the reserved vectdma_phyaddr.
 * @ addr: user space virtual address
 * @ size: the number of bytes should be allocated.
 * @ cached: cache property,cached or uncached
 * @ phy_nodeid: 0 stand for phy node0, 1 stand for phy node1
 */
asmlinkage unsigned long sys_smap_vectdma(unsigned long addr, unsigned long size, unsigned long cached, unsigned long phy_nodeid)
{
	printk("sys_smap_vectdma: addr=%p ,size=%lx\n", addr,size);
	unsigned long phy_addr,my_phy_addr,psize;
        unsigned int ret;
        struct vm_area_struct *vma = NULL;
        struct task_struct *tsk = current;
        struct mm_struct *mm = tsk->mm;
	extern unsigned int highmemsize;
	//addr &= 0xffffffff;
	//phy_addr = 0x9800000092a00000;
	phy_nodeid &= 0x1;
	if(phy_nodeid == 1)
		phy_addr = 0x9800100092a00000;
	if(phy_nodeid == 0)
		phy_addr = 0x9800000092a00000;

	int ipage = size/PAGE_SIZE;
	if(size%PAGE_SIZE)
	{
		printk("sys_smap_vectdma: size mod PAGE_SIZE is not equal zero!\n");
	    	return -EAGAIN;
	}	

        down_read(&mm->mmap_sem);
        vma = find_vma(mm,addr);
        up_read(&mm->mmap_sem);
       
	if(vma->vm_start > addr)
	{
	    printk("sys_smap_vectdma: vma->vm_start=0x%lx\n", vma->vm_start);
	    return -EAGAIN;
	}         

	if(vma->vm_start < addr )
	{
		split_vma(mm,vma,addr,1);
	}
	if(vma->vm_end > addr+ipage*PAGE_SIZE)
	{
		split_vma(mm,vma,(addr+ipage*PAGE_SIZE),0);
	}

        psize = vma->vm_end - vma->vm_start;
        my_phy_addr = virt_to_phys((void*)phy_addr);

        vma->vm_pgoff = my_phy_addr >> PAGE_SHIFT;

        vma->vm_flags |= VM_WRITE | VM_READ ;
	/* clear cache property first */
        vma->vm_page_prot.pgprot &= ~(0x7 << _CACHE_SHIFT);
	vma->vm_page_prot.pgprot |=_PAGE_PRESENT | _PAGE_GLOBAL | _PAGE_WRITE | _PAGE_READ;
	if(cached)
		vma->vm_page_prot.pgprot |= _CACHE_CACHABLE_NONCOHERENT;
	else
		vma->vm_page_prot.pgprot |= _CACHE_UNCACHED;
	printk("smap_vectdma:page_prot=%lx\n",vma->vm_page_prot.pgprot);

        ret = remap_pfn_range(vma,vma->vm_start,vma->vm_pgoff,psize,vma->vm_page_prot);
	if(ret)
		return -EAGAIN;
	return my_phy_addr;
}

/*
 * this function will remap the addr from the user space to the reserved vectdma_phyaddr.
 * @ addr: user space virtual address
 * @ size: the number of bytes should be allocated.
 * @ cached: cache property,cached or uncached
 * @ phy_nodeid: 0 stand for phy node0, 1 stand for phy node1
 */
asmlinkage unsigned long sys_smap_vectdma_uncache(unsigned long addr, unsigned long size, unsigned long cached, unsigned long phy_nodeid)
{
	unsigned long phy_addr,my_phy_addr,psize;
        unsigned int ret;
        struct vm_area_struct *vma = NULL;
        struct task_struct *tsk = current;
        struct mm_struct *mm = tsk->mm;
	extern unsigned int highmemsize;
	//addr &= 0xffffffff;
	//phy_addr = 0x9800000090200000;
	phy_nodeid &= 0x1;
	if(phy_nodeid == 1)
		phy_addr = 0x9800100090200000;
	if(phy_nodeid == 0)
		phy_addr = 0x9800000090200000;

	int ipage = size/PAGE_SIZE;
	if(size%PAGE_SIZE)
	{
		printk("sys_smap_vectdma_uncache: size mod PAGE_SIZE is not equal zero!\n");
	    	return -EAGAIN;
	}	

        down_read(&mm->mmap_sem);
        vma = find_vma(mm,addr);
        up_read(&mm->mmap_sem);
       
	if(vma->vm_start > addr)
	{
	    printk("sys_smap_vectdma_uncache: vma->vm_start=0x%lx\n", vma->vm_start);
	    return -EAGAIN;
	}         

	if(vma->vm_start < addr )
	{
		split_vma(mm,vma,addr,1);
	}
	if(vma->vm_end > addr+ipage*PAGE_SIZE)
	{
		split_vma(mm,vma,(addr+ipage*PAGE_SIZE),0);
	}

        psize = vma->vm_end - vma->vm_start;
        my_phy_addr = virt_to_phys((void*)phy_addr);

        vma->vm_pgoff = my_phy_addr >> PAGE_SHIFT;

        vma->vm_flags |= VM_WRITE | VM_READ ;
	/* clear cache property first */
        vma->vm_page_prot.pgprot &= ~(0x7 << _CACHE_SHIFT);
	vma->vm_page_prot.pgprot |=_PAGE_PRESENT | _PAGE_GLOBAL | _PAGE_WRITE | _PAGE_READ;
	if(cached)
		vma->vm_page_prot.pgprot |= _CACHE_CACHABLE_NONCOHERENT;
	else
		vma->vm_page_prot.pgprot |= _CACHE_UNCACHED;
	printk("smap_vectdma_uncache:page_prot=%lx\n",vma->vm_page_prot.pgprot);
        ret = remap_pfn_range(vma,vma->vm_start,vma->vm_pgoff,psize,vma->vm_page_prot);
	if(ret)
		return -EAGAIN;
	return my_phy_addr;
}

/*
 * this function will remap the addr from the user space to the reserved vectdma_phyaddr.
 * @ addr: user space virtual address
 * @ size: the number of bytes should be allocated.
 * @ cached: cache property,cached or uncached
 * @ phy_nodeid: 0 stand for phy node0, 1 stand for phy node1
 */
asmlinkage unsigned long sys_smap_vectdma_lock(unsigned long addr, unsigned long size, unsigned long cached, unsigned long phy_nodeid)
{
        printk("sys_smap_vectdma_lock: addr=%p ,size=%lx phy_nodeid=%llx\n", addr,size,phy_nodeid);
	unsigned long phy_addr,my_phy_addr,psize;
        unsigned int ret;
        struct vm_area_struct *vma = NULL;
        struct task_struct *tsk = current;
        struct mm_struct *mm = tsk->mm;
	extern unsigned int highmemsize;
	//addr &= 0xffffffff;
	//phy_addr = 0x9800000090000000;
	phy_nodeid &= 0x1;
	if(phy_nodeid == 1)
	{
		phy_addr = 0x9800100090000000;
	}
	else if(phy_nodeid == 0)
	{
		phy_addr = 0x9800000090000000;
	}

	int ipage = size/PAGE_SIZE;
	if(size%PAGE_SIZE)
	{
		printk("sys_smap_vectdma_lock: size mod PAGE_SIZE is not equal zero!\n");
	    	return -EAGAIN;
	}	

        down_read(&mm->mmap_sem);
        vma = find_vma(mm,addr);
        up_read(&mm->mmap_sem);
       
	if(vma->vm_start > addr)
	{
	    printk("sys_smap_vectdma_lock: vma->vm_start=0x%lx\n,addr=%p ,size=%lx\n", vma->vm_start,addr,size);
	    return -EAGAIN;
	}         

	if(vma->vm_start < addr )
	{
		split_vma(mm,vma,addr,1);
	}
	if(vma->vm_end > addr+ipage*PAGE_SIZE)
	{
		split_vma(mm,vma,(addr+ipage*PAGE_SIZE),0);
	}

        psize = vma->vm_end - vma->vm_start;
	printk("phy_addr=0x%llx\n",phy_addr);
        my_phy_addr = virt_to_phys((void*)phy_addr);
	printk("my_phy_addr=0x%llx\n",my_phy_addr);

        vma->vm_pgoff = my_phy_addr >> PAGE_SHIFT;

        vma->vm_flags |= VM_WRITE | VM_READ ;
	/* clear cache property first */
        vma->vm_page_prot.pgprot &= ~(0x7 << _CACHE_SHIFT);
	vma->vm_page_prot.pgprot |=_PAGE_PRESENT | _PAGE_GLOBAL | _PAGE_WRITE | _PAGE_READ;
	if(cached)
		vma->vm_page_prot.pgprot |= _CACHE_CACHABLE_NONCOHERENT;
	else
		vma->vm_page_prot.pgprot |= _CACHE_UNCACHED;
	printk("smap_vectdma_lock:page_prot=%lx\n",vma->vm_page_prot.pgprot);
        ret = remap_pfn_range(vma,vma->vm_start,vma->vm_pgoff,psize,vma->vm_page_prot);
	if(ret)
		return -EAGAIN;
	return my_phy_addr;
}

asmlinkage void sys_writeback_l2cache(unsigned long addr ,unsigned long size)
{
		//size &= 0xffffffff;
		addr &= 0xffffffff;
		size += 0x1f;
		size &= ~0x1f;
		printk("MMMMXXXFFF sys_writeback_l2cache: addr=%llx,size=%llx\n",addr,size);

		__asm__ volatile (
		".set mips64\n"
		".set noreorder\n"
		"daddiu $29,$29, -8\n"
		"sd	$18, 0x0($29)\n"
                "dli    $18,0x9800000000000000\n"
                "dadd   $18,$18,%0\n"
                "cache 0x17,0x0($18)\n"
                "1: daddi $18,0x20\n"
                "cache 0x17,0x0($18)\n"
                "addi  %1,%1,-32\n"
                "bnez  %1,1b\n"
                "nop\n"
		"ld	$18 ,0x0($29)\n"
		"daddiu $29,$29,8\n"
                "nop\n"
		".set reorder\n"
                :
                :"r"(addr), "r"(size)
                :"memory"
                );
}

asmlinkage void  sys_invalid_l2cache(unsigned long addr, unsigned long size)
{
		//size &= 0xffffffff;
		addr &= 0xffffffff;
		size += 0x1f;
		size &= ~0x1f;
		printk("sys_invalid_l2cache: addr=%llx,size=%llx\n",addr,size);

		__asm__ volatile (
		".set mips64\n"
		".set noreorder\n"
		"daddiu $29,$29, -8\n"
		"sd	$18, 0x0($29)\n"
                "dli    $18,0x9800000000000000\n"
                "dadd   $18,$18,%0\n"
                "cache 0x13,0x0($18)\n"
                "1: daddi $18,0x20\n"
                "cache 0x13,0x0($18)\n"
                "addi  %1,%1,-32\n"
                "bnez  %1,1b\n"
                "nop\n"
		"ld	$18 ,0x0($29)\n"
		"daddiu $29,$29,8\n"
		"nop\n"
		".set reorder\n"
                :
                :"r"(addr), "r"(size)
                :"memory"
                );
}

/*
 *If a process use vector dma,must call the function mannuly.
 */
asmlinkage void sys_vect_start(void)
{
        preempt_disable();
        set_thread_flag(TIF_USEDVPU);
	printk("sys_vect_start:TIF_USEDVPU=%d\n",test_thread_flag(TIF_USEDVPU));
        KSTK_STATUS(current)|=(ST0_CU1|ST0_CU2);
        preempt_enable();
}

asmlinkage void sys_hugemem_req_start(void)
{
	preempt_disable();
   	set_thread_flag(TIF_HUGEMEM);
	preempt_enable();
}

asmlinkage void sys_hugemem_req_end(void)
{
	preempt_disable();
 	clear_thread_flag(TIF_HUGEMEM);
	preempt_enable();
}
#endif