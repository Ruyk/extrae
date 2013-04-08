/*****************************************************************************\
 *                        ANALYSIS PERFORMANCE TOOLS                         *
 *                                   Extrae                                  *
 *              Instrumentation package for parallel applications            *
 *****************************************************************************
 *     ___     This library is free software; you can redistribute it and/or *
 *    /  __         modify it under the terms of the GNU LGPL as published   *
 *   /  /  _____    by the Free Software Foundation; either version 2.1      *
 *  /  /  /     \   of the License, or (at your option) any later version.   *
 * (  (  ( B S C )                                                           *
 *  \  \  \_____/   This library is distributed in hope that it will be      *
 *   \  \__         useful but WITHOUT ANY WARRANTY; without even the        *
 *    \___          implied warranty of MERCHANTABILITY or FITNESS FOR A     *
 *                  PARTICULAR PURPOSE. See the GNU LGPL for more details.   *
 *                                                                           *
 * You should have received a copy of the GNU Lesser General Public License  *
 * along with this library; if not, write to the Free Software Foundation,   *
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA          *
 * The GNU LEsser General Public License is contained in the file COPYING.   *
 *                                 ---------                                 *
 *   Barcelona Supercomputing Center - Centro Nacional de Supercomputacion   *
\*****************************************************************************/

/* -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=- *\
 | @file: $HeadURL$
 | @last_commit: $Date$
 | @version:     $Revision$
\* -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=- */
#include "common.h"

static char UNUSED rcsid[] = "$Id$";

#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#endif
#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif
#ifdef HAVE_STDIO_H
# include <stdio.h>
#endif
#ifdef HAVE_UCONTEXT_H
# include <ucontext.h>
#endif
#ifdef HAVE_SIGNAL_H
# include <signal.h>
#endif

#include "sampling.h"
#include "trace_macros.h"
#include "threadid.h"

#if defined(SAMPLING_SUPPORT)
int SamplingSupport = FALSE;
int EnabledSampling = FALSE;
#endif

int isSamplingEnabled(void)
{
#if defined(SAMPLING_SUPPORT)
	return EnabledSampling;
#else
	return FALSE;
#endif
}

void setSamplingEnabled (int enabled)
{
#if !defined(SAMPLING_SUPPORT)
	UNREFERENCED_PARAMETER(enabled);
#else
	EnabledSampling = (enabled != FALSE);
#endif
}


void Extrae_SamplingHandler (void* address)
{
	if (isSamplingEnabled())
	{
		UINT64 temps = Clock_getCurrentTime_nstore();
		SAMPLE_EVENT_HWC (temps, SAMPLING_EV, (unsigned long long) address);
		trace_callers (temps, 7, CALLER_SAMPLING);
	}
}

#if defined(IS_BGP_MACHINE) || defined(IS_BGQ_MACHINE)
/* BG/P  & BG/Q */
# if __WORDSIZE == 32
#  define UCONTEXT_REG(uc, reg) ((uc)->uc_mcontext.uc_regs->gregs[reg])
# else
#  define UCONTEXT_REG(uc, reg) ((uc)->uc_mcontext.gp_regs[reg])
# endif
# define PPC_REG_PC 32
#elif defined(IS_BGL_MACHINE)
# error "Don't know how to access the PC! Check if it is like BG/P"
#endif

static struct itimerval SamplingPeriod;
static int SamplingClockType;

static void TimeSamplingHandler (int sig, siginfo_t *siginfo, void *context)
{
	caddr_t pc;
#if defined(OS_FREEBSD) || defined(OS_DARWIN)
	ucontext_t *uc = (ucontext_t *) context;
#else
	struct ucontext *uc = (struct ucontext *) context;
	struct sigcontext *sc = (struct sigcontext *) &uc->uc_mcontext;
#endif

	UNREFERENCED_PARAMETER(sig);
	UNREFERENCED_PARAMETER(siginfo);

#if defined(IS_BGP_MACHINE) || defined(IS_BGQ_MACHINE)
	pc = (caddr_t)UCONTEXT_REG(uc, PPC_REG_PC);
#elif defined(OS_LINUX)
# if defined(ARCH_IA32) && !defined(ARCH_IA32_x64)
  pc = (caddr_t)sc->eip;
# elif defined(ARCH_IA32) && defined(ARCH_IA32_x64)
	pc = (caddr_t)sc->rip;
# elif defined(ARCH_IA64)
	pc = (caddr_t)sc->sc_ip;
# elif defined(ARCH_PPC)
	pc = (caddr_t)sc->regs->nip;
# elif defined(ARCH_ARM)
	pc = (caddr_t)sc->arm_pc;
# else
#  error "Don't know how to get the PC for this architecture in Linux!"
# endif
#elif defined(OS_FREEBSD)
# if defined(ARCH_IA32) && !defined(ARCH_IA32_x64)
	pc = (caddr_t)(uc->uc_mcontext.mc_eip);
# elif defined (ARCH_IA32) && defined(ARCH_IA32_x64)
	pc = (caddr_t)(uc->uc_mcontext.mc_rip);
# else
#  error "Don't know how to get the PC for this architecture in FreeBSD!"
# endif
#elif defined(OS_DARWIN)
# if defined(ARCH_IA32) && !defined(ARCH_IA32_x64)
	pc = (caddr_t)((uc->uc_mcontext)->__ss.__eip);
# elif defined (ARCH_IA32) && defined(ARCH_IA32_x64)
	pc = (caddr_t)((uc->uc_mcontext)->__ss.__rip);
# else
#  error "Don't know how to get the PC for this architecture in Darwin!"
# endif
#else
# error "Don't know how to get the PC for this OS!"
#endif

	Extrae_SamplingHandler ((void*) pc);

	/* Set next timer! */
	setitimer (SamplingClockType ,&SamplingPeriod, NULL);
}


void setTimeSampling (unsigned long long period, int sampling_type)
{
	int signum;
	int ret;
	struct sigaction act;

	memset (&act, 0, sizeof(act));

	ret = sigemptyset(&act.sa_mask);
	if (ret != 0)
	{
		fprintf (stderr, PACKAGE_NAME": Error! Sampling error: %s\n", strerror(ret));
		return;
	}
	ret = sigaddset(&act.sa_mask, SIGALRM);
	if (ret != 0)
	{
		fprintf (stderr, PACKAGE_NAME": Error! Sampling error: %s\n", strerror(ret));
		return;
	}

	/* The period is given in nanoseconds */
	period = period / 1000;

	SamplingPeriod.it_interval.tv_sec = 0;
	SamplingPeriod.it_interval.tv_usec = 0;
	SamplingPeriod.it_value.tv_sec = period / 1000000;
	SamplingPeriod.it_value.tv_usec = period % 1000000;

	if (sampling_type == SAMPLING_TIMING_VIRTUAL)
	{
		SamplingClockType = ITIMER_VIRTUAL;
		signum = SIGVTALRM;
	}
	else if (sampling_type == SAMPLING_TIMING_PROF)
	{
		SamplingClockType = ITIMER_PROF;
		signum = SIGPROF;
	}
	else
	{
		SamplingClockType = ITIMER_REAL;
		signum = SIGALRM;
	}

	act.sa_sigaction = TimeSamplingHandler;
	act.sa_flags = SA_SIGINFO;

	ret = sigaction (signum, &act, NULL);
	if (ret != 0)
	{
		fprintf (stderr, PACKAGE_NAME": Error! Sampling error: %s\n", strerror(ret));
		return;
	}

	setitimer (SamplingClockType, &SamplingPeriod, NULL);
}
