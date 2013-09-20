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

#if defined(HAVE_XML2)

#include <libxml/xmlmemory.h>
#include <libxml/parser.h>

#ifdef HAVE_STRING_H
# include <string.h>
#endif
#ifdef HAVE_STDIO_H
# include <stdio.h>
#endif
#ifdef HAVE_STDLIB_H
# include <stdlib.h>
#endif
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#ifdef HAVE_FCNTL_H
# include <fcntl.h>
#endif
#ifdef HAVE_SIGNAL_H
# include <signal.h>
#endif
#if defined(MPI_SUPPORT)
# ifdef HAVE_MPI_H
#  include <mpi.h>
# endif
#endif
#if defined(PACX_SUPPORT)
# include <pacx.h>
#endif

#include "utils.h"
#include "hwc.h"
#include "xml-parse.h"
#include "wrapper.h"
#include "signals.h"
#if defined(MPI_SUPPORT)
# include "mpi_wrapper.h"
#endif
#if defined(OMP_SUPPORT)
# include "omp_wrapper.h"
#endif
#if defined(IS_CELL_MACHINE)
# include "cell_wrapper.h"
#endif
#include "UF_gcc_instrument.h"
#include "UF_xl_instrument.h"
#if defined(HAVE_ONLINE)
# include "OnlineConfig.h"
#endif
#if defined(OMP_SUPPORT)
# include "omp_probe.h"
#endif
#if defined(SAMPLING_SUPPORT)
# include "sampling.h"
#endif
#if defined(PTHREAD_SUPPORT)
# include "pthread_probe.h"
#endif

/* Some global (but local in the module) variables */
static char *temporal_d = NULL, *final_d = NULL;
static int TracePrefixFound = FALSE;

static const xmlChar *xmlYES = (xmlChar*) "yes";
static const xmlChar *xmlNO = (xmlChar*) "no";
static const xmlChar *xmlCOMMENT = (xmlChar*) "COMMENT";
static const xmlChar *xmlTEXT = (xmlChar*) "text";

static const xmlChar XML_ENVVAR_CHARACTER = (xmlChar) '$';

/* Free memory if not null */
#define XML_FREE(ptr) \
	if (ptr != NULL) xmlFree(ptr);

/* master fprintf :) */
#define mfprintf \
	if (rank == 0) fprintf 

/**********************************************************************
    WRAPPERS for
     xmlNodeListGetString
    and
     xmlGetProp

    If they reference a environment variable (using XML_ENVVAR_CHARACTER)
    get its value from the environment.
**********************************************************************/

static xmlChar * deal_xmlChar_env (int rank, xmlChar *str)
{
	xmlChar *tmp;
	int i;
	int initial = 0;
	int sublen = 0;
	int length = xmlStrlen (str);
	int final = length;

	/* First get rid of the leading and trailing white spaces */
	for (i = 0; i < length; i++)
		if (!is_Whitespace (str[i]))
			break;
	initial = i;
	for (; final-1 >= i; final--)
		if (!is_Whitespace (str[final-1]))
			break;

	sublen = final - initial;

	tmp = xmlStrsub (str, initial, sublen);

	if (sublen > 0)
	{
		/* If the string is wrapped by XML_ENVVAR_CHARACTER, perform a getenv and
		   return its result */
		if (sublen > 1 && tmp[0] == XML_ENVVAR_CHARACTER && tmp[sublen-1] == XML_ENVVAR_CHARACTER)
		{
			char tmp2[sublen];
			memset (tmp2, 0, sublen);
			strncpy (tmp2, &tmp[1], sublen-2);

			if (getenv (tmp2) == NULL)
			{
				mfprintf (stderr, PACKAGE_NAME": Environment variable %s is not defined!\n", tmp2);
				return NULL;
			}
			else
			{
				if (strlen(getenv(tmp2)) == 0)
				{
					mfprintf (stderr, PACKAGE_NAME": Environment variable %s is set but empty!\n", tmp2);
					return NULL;
				}
				else
					return xmlCharStrdup (getenv(tmp2));
			}
		}
		else
			return tmp;
	}
	else
		return tmp;
}

static xmlChar * xmlNodeListGetString_env (int rank, xmlDocPtr doc, xmlNodePtr list, int inLine)
{
	xmlChar *tmp;

	tmp = xmlNodeListGetString (doc, list, inLine);
	if (tmp != NULL)
	{
		xmlChar *tmp2;
		tmp2 = deal_xmlChar_env (rank, tmp);
		XML_FREE(tmp);
		return tmp2;
	}
	else
		return NULL;
}

static xmlChar* xmlGetProp_env (int rank, xmlNodePtr node, xmlChar *attribute)
{
	xmlChar *tmp;

	tmp = xmlGetProp (node, attribute);
	if (tmp != NULL)
	{
		xmlChar *tmp2;
		tmp2 = deal_xmlChar_env (rank, tmp);
		XML_FREE(tmp);
		return tmp2;
	}
	else
		return NULL;	
}

#if defined(CUDA_SUPPORT)
/* Configure CUDA related parameters */
static void Parse_XML_CUDA (int rank, xmlDocPtr xmldoc, xmlNodePtr current_tag)
{
	xmlNodePtr tag;

	UNREFERENCED_PARAMETER(xmldoc);

	/* Parse all TAGs, and annotate them to use them later */
	tag = current_tag->xmlChildrenNode;
	while (tag != NULL)
	{
		/* Skip coments */
		if (!xmlStrcasecmp (tag->name, xmlCOMMENT) || !xmlStrcmp (tag->name, xmlTEXT))
		{
		}
		/* Shall we gather counters in the CUDA calls? */
		else if (!xmlStrcasecmp (tag->name, TRACE_COUNTERS))
		{
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			tracing_cuda = enabled != NULL && !xmlStrcasecmp (enabled, xmlYES);
			XML_FREE(enabled);
		}
		else
		{
			mfprintf (stderr, PACKAGE_NAME": XML unknown tag '%s' at <CUDA> level\n", tag->name);
		}

		tag = tag->next;
	}
}
#endif

#if defined(OPENCL_SUPPORT)
/* Configure OpenCL related parameters */
static void Parse_XML_OpenCL (int rank, xmlDocPtr xmldoc, xmlNodePtr current_tag)
{
	xmlNodePtr tag;

	UNREFERENCED_PARAMETER(xmldoc);

	/* Parse all TAGs, and annotate them to use them later */
	tag = current_tag->xmlChildrenNode;
	while (tag != NULL)
	{
		/* Skip coments */
		if (!xmlStrcasecmp (tag->name, xmlCOMMENT) || !xmlStrcmp (tag->name, xmlTEXT))
		{
		}
		/* Shall we gather counters in the MPI calls? */
		else if (!xmlStrcasecmp (tag->name, TRACE_COUNTERS))
		{
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			tracing_opencl = enabled != NULL && !xmlStrcasecmp (enabled, xmlYES);
			XML_FREE(enabled);
		}
		else
		{
			mfprintf (stderr, PACKAGE_NAME": XML unknown tag '%s' at <OPENCL> level\n", tag->name);
		}

		tag = tag->next;
	}
}
#endif

#if defined(MPI_SUPPORT)
/* Configure MPI related parameters */
static void Parse_XML_MPI (int rank, xmlDocPtr xmldoc, xmlNodePtr current_tag)
{
	xmlNodePtr tag;

	UNREFERENCED_PARAMETER(xmldoc);

	/* Parse all TAGs, and annotate them to use them later */
	tag = current_tag->xmlChildrenNode;
	while (tag != NULL)
	{
		/* Skip coments */
		if (!xmlStrcasecmp (tag->name, xmlCOMMENT) || !xmlStrcmp (tag->name, xmlTEXT))
		{
		}
		/* Shall we gather counters in the MPI calls? */
		else if (!xmlStrcasecmp (tag->name, TRACE_COUNTERS))
		{
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			tracejant_hwc_mpi = enabled != NULL && !xmlStrcasecmp (enabled, xmlYES);
#if USE_HARDWARE_COUNTERS
			mfprintf (stdout, PACKAGE_NAME": MPI routines will %scollect HW counters information.\n", tracejant_hwc_mpi?"":"NOT ");
#else
			mfprintf (stdout, PACKAGE_NAME": <%s> tag at <MPI> level will be ignored. This library does not support CPU HW.\n", TRACE_COUNTERS);
			tracejant_hwc_mpi = FALSE;
#endif
			XML_FREE(enabled);
		}
		else
		{
			mfprintf (stderr, PACKAGE_NAME": XML unknown tag '%s' at <MPI> level\n", tag->name);
		}

		tag = tag->next;
	}
}
#endif

#if defined(PACX_SUPPORT)
/* Configure PACX related parameters */
static void Parse_XML_PACX (int rank, xmlDocPtr xmldoc, xmlNodePtr current_tag)
{
	xmlNodePtr tag;

	/* Parse all TAGs, and annotate them to use them later */
	tag = current_tag->xmlChildrenNode;
	while (tag != NULL)
	{
		/* Skip coments */
		if (!xmlStrcasecmp (tag->name, xmlCOMMENT) || !xmlStrcmp (tag->name, xmlTEXT))
		{
		}
		/* Shall we gather counters in the PACX calls? */
		else if (!xmlStrcasecmp (tag->name, TRACE_COUNTERS))
		{
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			tracejant_hwc_pacx = enabled != NULL && !xmlStrcasecmp (enabled, xmlYES);
#if USE_HARDWARE_COUNTERS
			mfprintf (stdout, PACKAGE_NAME": PACX routines will %scollect HW counters information.\n", tracejant_hwc_pacx?"":"NOT ");
#else
			mfprintf (stdout, PACKAGE_NAME": <%s> tag at <PACX> level will be ignored. This library does not support CPU HW.\n", TRACE_COUNTERS);
			tracejant_hwc_pacx = FALSE;
#endif
			XML_FREE(enabled);
		}
		else
		{
			mfprintf (stderr, PACKAGE_NAME": XML unknown tag '%s' at <PACX> level\n", tag->name);
		}

		tag = tag->next;
	}
}
#endif


#if defined(SAMPLING_SUPPORT)
static void Parse_XML_Sampling (int rank, xmlNodePtr current_tag)
{
	xmlChar *period = xmlGetProp_env (rank, current_tag, TRACE_PERIOD);
	xmlChar *variability = xmlGetProp_env (rank, current_tag, TRACE_VARIABILITY);
	xmlChar *clocktype = xmlGetProp_env (rank, current_tag, TRACE_TYPE);

	if (period != NULL)
	{
		unsigned long long sampling_period = getTimeFromStr (period,
			"<sampling period=\"..\" />", rank);
		unsigned long long sampling_variability = 0;
		if (variability != NULL)
			sampling_variability = getTimeFromStr (variability, "<sampling variability=\"..\" />", rank);

		if (sampling_period != 0)
		{
			if (clocktype != NULL)
			{
				if (!xmlStrcasecmp(clocktype, "DEFAULT"))
					setTimeSampling (sampling_period, sampling_variability, SAMPLING_TIMING_DEFAULT);
				else if (!xmlStrcasecmp (clocktype, "REAL"))
					setTimeSampling (sampling_period, sampling_variability, SAMPLING_TIMING_REAL);
				else if (!xmlStrcasecmp (clocktype, "VIRTUAL"))
					setTimeSampling (sampling_period, sampling_variability, SAMPLING_TIMING_VIRTUAL);
				else if (!xmlStrcasecmp (clocktype, "PROF"))
					setTimeSampling (sampling_period, sampling_variability, SAMPLING_TIMING_PROF);
				else 
					mfprintf (stderr, "Extrae: Warning! Value '%s' <sampling type=\"..\" /> is unrecognized. Using default clock.\n", clocktype);					
			}
			else	
				setTimeSampling (sampling_period, sampling_variability, SAMPLING_TIMING_DEFAULT);

			if (rank == 0)
				fprintf (stdout, "Extrae: Sampling enabled with a period of %lld microseconds and a variability of %lld microseconds.\n", sampling_period/1000, sampling_variability/1000);
		}
		else
		{
			mfprintf (stderr, "Extrae: Warning! Value '%s' for <sampling period=\"..\" /> is unrecognized\n", period);
		}
	}

	XML_FREE(period);
	XML_FREE(variability);
	XML_FREE(clocktype);
}
#endif /* SAMPLING_SUPPORT */

/* Configure Callers related parameters */
static void Parse_XML_Callers (int rank, xmlDocPtr xmldoc, xmlNodePtr current_tag)
{
	xmlNodePtr tag;

	UNREFERENCED_PARAMETER(xmldoc);

	/* Parse all TAGs, and annotate them to use them later */
	tag = current_tag->xmlChildrenNode;
	while (tag != NULL)
	{
		/* Skip coments */
		if (!xmlStrcasecmp (tag->name, xmlTEXT) || !xmlStrcmp (tag->name, xmlCOMMENT))
		{
		}
		/* Must the tracing facility obtain information about PACX callers? */
		else if (!xmlStrcasecmp (tag->name, TRACE_MPI))
		{
#if defined(MPI_SUPPORT)
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
			{
				char *callers = (char*) xmlNodeListGetString_env (rank, xmldoc, tag->xmlChildrenNode, 1);
				if (callers != NULL)
					Parse_Callers (rank, callers, CALLER_MPI);
				XML_FREE(callers);
			}
			XML_FREE(enabled);
#else
			mfprintf (stdout, PACKAGE_NAME": <%s> tag at <Callers> level will be ignored. This library does not support MPI.\n", TRACE_MPI);
#endif
		}
		else if (!xmlStrcasecmp (tag->name, TRACE_PACX))
		{
#if defined(PACX_SUPPORT)
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
			{
				char *callers = (char*) xmlNodeListGetString_env (rank, xmldoc, tag->xmlChildrenNode, 1);
				if (callers != NULL)
					Parse_Callers (rank, callers, CALLER_MPI);
				XML_FREE(callers);
			}
			XML_FREE(enabled);
#else
			mfprintf (stdout, PACKAGE_NAME": <%s> tag at <Callers> level will be ignored. This library does not support PACX.\n", TRACE_PACX);
#endif
		}
		/* Must the tracing facility obtain information about callers at sample points? */
		else if (!xmlStrcasecmp (tag->name, TRACE_SAMPLING))
		{
#if defined(SAMPLING_SUPPORT)
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
			{
				char *callers = (char*) xmlNodeListGetString_env (rank, xmldoc, tag->xmlChildrenNode, 1);
				if (callers != NULL)
					Parse_Callers (rank, callers, CALLER_SAMPLING);
				XML_FREE(callers);
			}
			XML_FREE(enabled);
#else
			mfprintf (stdout, PACKAGE_NAME": <%s> tag at <Callers> level will be ignored. This library does not support SAMPLING.\n", TRACE_SAMPLING);
#endif
		}
		else
		{
			mfprintf (stderr, PACKAGE_NAME": XML unknown tag '%s' at <callers> level\n", tag->name);
		}

		tag = tag->next;
	}
}

#if defined(IS_CELL_MACHINE)
/* Configure SPU related parameters */
static void Parse_XML_CELL (int rank, xmlDocPtr xmldoc, xmlNodePtr current_tag)
{
	xmlNodePtr tag;

	/* Parse all TAGs, and annotate them to use them later */
	tag = current_tag->xmlChildrenNode;
	while (tag != NULL)
	{
		/* Skip coments */
		if (!xmlStrcasecmp (tag->name, xmlTEXT) || !xmlStrcmp (tag->name, xmlCOMMENT))
		{
		}
		/* Buffer size of the SPU tracing unit  */
		else if (!xmlStrcasecmp (tag->name, TRACE_SPU_BUFFERSIZE))
		{
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
			{
				char *str = (char*) xmlNodeListGetString_env (rank, xmldoc, tag->xmlChildrenNode, 1);
				spu_buffer_size = (str!=NULL)?atoi (str):-1;
				if (spu_buffer_size < 10)
				{
					mfprintf (stderr, PACKAGE_NAME": SPU tracing buffer size '%d' too small. Using default SPU buffer size '%d'.\n", spu_buffer_size, DEFAULT_SPU_BUFFER_SIZE);
					spu_buffer_size = DEFAULT_SPU_BUFFER_SIZE;
				}
				else
				{
					mfprintf (stdout, PACKAGE_NAME": SPU tracing buffer size is %d events.\n", spu_buffer_size);
				}
				XML_FREE(str);
			}
			XML_FREE(enabled); 
		}
		/* DMA tag configuration for bulk transferences */
		else if (!xmlStrcasecmp (tag->name, TRACE_SPU_DMATAG))
		{
#ifndef SPU_USES_WRITE
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
			{
				char *str = (char*) xmlNodeListGetString_env (rank, xmldoc, tag->xmlChildrenNode, 1);
				spu_dma_channel = (str!=NULL)?atoi (str):-1;
				if ((spu_dma_channel < 0) || (spu_dma_channel > 31))
				{
					mfprintf (stderr, PACKAGE_NAME": Invalid DMA channel '%s'. Using default channel.\n", str);
					spu_dma_channel = DEFAULT_DMA_CHANNEL;
				}
				else
				{
					mfprintf (stdout, PACKAGE_NAME": Using DMA channel %d for memory transferences.\n", spu_dma_channel);
				}
				XML_FREE(str);
			}
			XML_FREE(enabled); 
#else
			mfprintf (stdout, PACKAGE_NAME": SPUs will write directly to disk. Ignoring tag %s\n", TRACE_SPU_DMATAG);
#endif
		}
		/* SPU hosted file size limit */
		else if (!xmlStrcasecmp (tag->name, TRACE_SPU_FILESIZE))
		{
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
			{
				char *str = (char*) xmlNodeListGetString_env (rank, xmldoc, tag->xmlChildrenNode, 1);
				spu_file_size = (str!=NULL)?atoi (str):-1;
				if (spu_file_size < 1)
				{
					mfprintf (stderr, PACKAGE_NAME": SPU tracing buffer size '%d' too small. Using default SPU buffer size '%d' mbytes.\n", spu_file_size, DEFAULT_SPU_FILE_SIZE); 
					spu_file_size = DEFAULT_SPU_FILE_SIZE;
				}
				else
				{
					mfprintf (stdout, PACKAGE_NAME": SPU tracing file size limit is %d mbytes.\n", spu_file_size);
				}
				XML_FREE(str);
			}
			XML_FREE(enabled); 
		}
		else
		{
			mfprintf (stderr, PACKAGE_NAME": XML unknown tag '%s' at <CELL> level\n", tag->name);
		}

		tag = tag->next;
	}
}
#endif /* IS_CELL_MACHINE */

/* Configure Bursts related parameters */
static void Parse_XML_Bursts (int rank, xmlDocPtr xmldoc, xmlNodePtr current_tag)
{
	xmlNodePtr tag;

	/* Parse all TAGs, and annotate them to use them later */
	tag = current_tag->xmlChildrenNode;
	while (tag != NULL)
	{
		/* Skip coments */
		if (!xmlStrcasecmp (tag->name, xmlTEXT) || !xmlStrcmp (tag->name, xmlCOMMENT))
		{
		}
		/* Which is the threshold for the Bursts? */
		else if (!xmlStrcasecmp (tag->name, TRACE_THRESHOLD))
		{
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
			{
				char *str = (char*) xmlNodeListGetString_env (rank, xmldoc, tag->xmlChildrenNode, 1);
				if (str != NULL)
				{
					TMODE_setBurstsThreshold (getTimeFromStr (str, TRACE_THRESHOLD, rank));
				}
				XML_FREE(str);
			}
			XML_FREE(enabled);
		}
		else if (!xmlStrcasecmp (tag->name, TRACE_MPI_STATISTICS))
		{
#if defined(MPI_SUPPORT)
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			TMODE_setBurstsStatistics (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES));
			XML_FREE(enabled);
#else
			mfprintf (stderr, PACKAGE_NAME": <%s> tag at <%s> level will be ignored. This library does not support MPI.\n", TRACE_MPI_STATISTICS, TRACE_BURSTS);
#endif
		}
		else if (!xmlStrcasecmp (tag->name, TRACE_PACX_STATISTICS))
		{
#if defined(PACX_SUPPORT)
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			TMODE_setBurstsStatistics (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES));
			XML_FREE(enabled);
#else
			mfprintf (stderr, PACKAGE_NAME": <%s> tag at <%s> level will be ignored. This library does not support PACX.\n", TRACE_PACX_STATISTICS, TRACE_BURSTS);
#endif
		}
		else
		{
			mfprintf (stderr, PACKAGE_NAME": XML unknown tag '%s' at <%s> level\n", tag->name, TRACE_BURSTS);
		}

		tag = tag->next;
	}
}


/* Configure UserFunction related parameters */
static void Parse_XML_UF (int rank, xmlNodePtr current_tag)
{
	xmlNodePtr tag;
	char *list = (char*) xmlGetProp_env (rank, current_tag, TRACE_LIST);
	if (list != NULL)
	{
		InstrumentUFroutines_XL (rank, list);
		InstrumentUFroutines_GCC (rank, list);

		XML_FREE(list);
	}

	/* Parse all TAGs, and annotate them to use them later */
	tag = current_tag->xmlChildrenNode;
	while (tag != NULL)
	{
		/* Skip coments */
		if (!xmlStrcasecmp (tag->name, xmlTEXT) || !xmlStrcmp (tag->name, xmlCOMMENT))
		{
		}
		/* Shall we gather counters in the UF calls? */
		else if (!xmlStrcasecmp (tag->name, TRACE_COUNTERS))
		{
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			tracejant_hwc_uf = ((enabled != NULL && !xmlStrcasecmp (enabled, xmlYES)));
#if USE_HARDWARE_COUNTERS
			mfprintf (stdout, PACKAGE_NAME": User Function routines will %scollect HW counters information.\n", tracejant_hwc_uf?"":"NOT ");
#else
			mfprintf (stdout, PACKAGE_NAME": <%s> tag at <user-functions> level will be ignored. This library does not support CPU HW.\n", TRACE_COUNTERS);
			tracejant_hwc_uf = FALSE;
#endif
			XML_FREE(enabled);
		}
		else
		{
			mfprintf (stderr, PACKAGE_NAME": XML unknown tag '%s' at <UserFunctions> level\n", tag->name);
		}

		tag = tag->next;
	}
}

#if defined(OMP_SUPPORT) || defined(SMPSS_SUPPORT)
/* Configure OpenMP related parameters */
static void Parse_XML_OMP (int rank, xmlDocPtr xmldoc, xmlNodePtr current_tag)
{
	xmlNodePtr tag;
	UNREFERENCED_PARAMETER(xmldoc);

	/* Parse all TAGs, and annotate them to use them later */
	tag = current_tag->xmlChildrenNode;
	while (tag != NULL)
	{
		/* Skip coments */
		if (!xmlStrcasecmp (tag->name, xmlTEXT) || !xmlStrcmp (tag->name, xmlCOMMENT))
		{
		}
		/* Shall we instrument openmp lock routines? */
		else if (!xmlStrcasecmp (tag->name, TRACE_OMP_LOCKS))
		{
#if defined(OMP_SUPPORT)
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			setTrace_OMPLocks ((enabled != NULL && !xmlStrcasecmp (enabled, xmlYES)));
			XML_FREE(enabled);
#endif
		}
		/* Shall we gather counters in the UF calls? */
		else if (!xmlStrcasecmp (tag->name, TRACE_COUNTERS))
		{
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			tracejant_hwc_omp = ((enabled != NULL && !xmlStrcasecmp (enabled, xmlYES)));
#if USE_HARDWARE_COUNTERS
			mfprintf (stdout, PACKAGE_NAME": OpenMP routines will %scollect HW counters information.\n", tracejant_hwc_omp?"":"NOT");
#else
			mfprintf (stdout, PACKAGE_NAME": <%s> tag at <OpenMP> level will be ignored. This library does not support CPU HW.\n", TRACE_COUNTERS);
			tracejant_hwc_omp = FALSE;
#endif
			XML_FREE(enabled);
		}
		else
		{
			mfprintf (stderr, PACKAGE_NAME": XML unknown tag '%s' at <OpenMP> level\n", tag->name);
		}

		tag = tag->next;
	}
}
#endif

#if defined(PTHREAD_SUPPORT)
/* Configure OpenMP related parameters */
static void Parse_XML_PTHREAD (int rank, xmlDocPtr xmldoc, xmlNodePtr current_tag)
{
	xmlNodePtr tag;
	UNREFERENCED_PARAMETER(xmldoc);

	/* Parse all TAGs, and annotate them to use them later */
	tag = current_tag->xmlChildrenNode;
	while (tag != NULL)
	{
		/* Skip coments */
		if (!xmlStrcasecmp (tag->name, xmlTEXT) || !xmlStrcmp (tag->name, xmlCOMMENT))
		{
		}
		/* Shall we instrument openmp lock routines? */
		else if (!xmlStrcasecmp (tag->name, TRACE_PTHREAD_LOCKS))
		{
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			Extrae_pthread_instrument_locks ((enabled != NULL && !xmlStrcasecmp (enabled, xmlYES)));
			XML_FREE(enabled);
		}
		/* Shall we gather counters in the UF calls? */
		else if (!xmlStrcasecmp (tag->name, TRACE_COUNTERS))
		{
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			tracejant_hwc_pthread = ((enabled != NULL && !xmlStrcasecmp (enabled, xmlYES)));
#if USE_HARDWARE_COUNTERS
			mfprintf (stdout, PACKAGE_NAME": pthread routines will %scollect HW counters information.\n", tracejant_hwc_omp?"":"NOT ");
#else
			mfprintf (stdout, PACKAGE_NAME": <%s> tag at <pthread> level will be ignored. This library does not support CPU HW.\n", TRACE_COUNTERS);
			tracejant_hwc_omp = FALSE;
#endif
			XML_FREE(enabled);
		}
		else
		{
			mfprintf (stderr, PACKAGE_NAME": XML unknown tag '%s' at <pthread> level\n", tag->name);
		}

		tag = tag->next;
	}
}
#endif


/* Configure storage related parameters */
static void Parse_XML_Storage (int rank, xmlDocPtr xmldoc, xmlNodePtr current_tag)
{
	xmlNodePtr tag;

	/* Parse all TAGs, and annotate them to use them later */
	tag = current_tag->xmlChildrenNode;
	while (tag != NULL)
	{
		/* Skip coments */
		if (!xmlStrcasecmp (tag->name, xmlTEXT) || !xmlStrcmp (tag->name, xmlCOMMENT))
		{
		}
		/* Does the user want to change the file size? */
		else if (!xmlStrcasecmp (tag->name, TRACE_SIZE))
		{
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
			{
				char *fsize = (char*) xmlNodeListGetString_env (rank, xmldoc, tag->xmlChildrenNode, 1);
				if (fsize != NULL)
				{
					file_size = atoi(fsize);
					if (file_size <= 0)
					{
						mfprintf (stderr, PACKAGE_NAME": Invalid file size value.\n");
					}
					else if (file_size > 0)
					{
						mfprintf (stdout, PACKAGE_NAME": Intermediate file size set to %d Mbytes.\n", file_size);
					}
				}
				XML_FREE(fsize);
			}
			XML_FREE(enabled);
		}
		/* Where must we store the intermediate files? DON'T FREE it's used below */
		else if (!xmlStrcasecmp (tag->name, TRACE_DIR))
		{
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
				temporal_d = (char*) xmlNodeListGetString_env (rank, xmldoc, tag->xmlChildrenNode, 1);
			XML_FREE(enabled);
		}
		/* Where must we store the final intermediate files?  DON'T FREE it's used below */
		else if (!xmlStrcasecmp (tag->name, TRACE_FINAL_DIR))
		{
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
				final_d = (char*) xmlNodeListGetString_env (rank, xmldoc, tag->xmlChildrenNode, 1);
			XML_FREE(enabled);
		}
#if defined(MPI_SUPPORT)
		/* Must the tracing gather the MPITs into one process? */
		else if (!xmlStrcasecmp (tag->name, TRACE_GATHER_MPITS))
		{
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			mpit_gathering_enabled = ((enabled != NULL && !xmlStrcasecmp (enabled, xmlYES)));
			mfprintf (stdout, PACKAGE_NAME": All MPIT files will %s be gathered at the end of the execution!\n", mpit_gathering_enabled?"":"NOT");
			XML_FREE(enabled);
		}
#endif
		/* Obtain the MPIT prefix */
		else if (!xmlStrcasecmp (tag->name, TRACE_PREFIX))
		{
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
			{
				char *p_name = (char*) xmlNodeListGetString_env (rank, xmldoc, tag->xmlChildrenNode, 1);
				strncpy (PROGRAM_NAME, p_name, sizeof(PROGRAM_NAME));
				TracePrefixFound = TRUE;
				XML_FREE(p_name);
			}
			else
			{
				/* If not enabled, just put TRACE as the program name */
				strncpy (PROGRAM_NAME, "TRACE", strlen("TRACE")+1);
				TracePrefixFound = TRUE;
			}
			XML_FREE(enabled);
		}
		else
		{
			mfprintf (stderr, PACKAGE_NAME": XML unknown tag '%s' at <Storage> level\n", tag->name);
		}

		tag = tag->next;
	}
}

/* Configure buffering related parameters */
static void Parse_XML_Buffer (int rank, xmlDocPtr xmldoc, xmlNodePtr current_tag)
{
	xmlNodePtr tag;

	/* Parse all TAGs, and annotate them to use them later */
	tag = current_tag->xmlChildrenNode;
	while (tag != NULL)
	{
		/* Skip coments */
		if (!xmlStrcasecmp (tag->name, xmlTEXT) || !xmlStrcmp (tag->name, xmlCOMMENT))
		{
		}
		/* Must we limit the buffer size? */
		else if (!xmlStrcasecmp (tag->name, TRACE_SIZE))
		{
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
			{
				char *bsize = (char*) xmlNodeListGetString_env (rank, xmldoc, tag->xmlChildrenNode, 1);
				if (bsize != NULL)
				{
					int size = atoi(bsize);
					buffer_size = (size<=0)?EVT_NUM:size;
					mfprintf (stdout, PACKAGE_NAME": Tracing buffer can hold %d events\n", buffer_size);
				}
				XML_FREE(bsize);
			}
			XML_FREE(enabled);
		}
		/* Do we activate the circular buffering ? */
		else if (!xmlStrcasecmp (tag->name, TRACE_CIRCULAR))
		{
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
			{
				mfprintf (stdout, PACKAGE_NAME": Circular buffer %s.\n", circular_buffering?"enabled":"disabled");
				circular_buffering = 1;
			}
			XML_FREE(enabled);
		}
		else
		{
			mfprintf (stderr, PACKAGE_NAME": XML unknown tag '%s' at <Buffer> level\n", tag->name);
		}

		tag = tag->next;
	}
}

#if USE_HARDWARE_COUNTERS
static void Parse_XML_Counters_CPU_Sampling (int rank, xmlDocPtr xmldoc, xmlNodePtr current, int *num, char ***counters, unsigned long long **periods)
{
	xmlNodePtr set_tag;
	xmlChar *enabled;
	int num_sampling_hwc, i;
	unsigned long long *t_periods = NULL;
	char **t_counters = NULL;

	/* HSG ATTENTION!
	   Originally, the XML tag for period was 'frequency!". For now, we check for
	   both period (first) and frequency (second), just to be compatible with old
	   XMLs!
	*/

	/* Parse all HWC sets, and annotate them to use them later */
	set_tag = current->xmlChildrenNode;
	num_sampling_hwc = 0;
	while (set_tag != NULL)
	{
		/* Skip coments */
		if (!xmlStrcasecmp (set_tag->name, xmlTEXT) || !xmlStrcmp (set_tag->name, xmlCOMMENT))
		{
		}
		else if (!xmlStrcasecmp (set_tag->name, TRACE_SAMPLING))
		{
			enabled = xmlGetProp_env (rank, set_tag, TRACE_ENABLED);
			if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
			{
				char *tmp = (char*) xmlGetProp_env (rank, set_tag, TRACE_PERIOD);
				if (tmp == NULL)
					tmp = (char*) xmlGetProp_env (rank, set_tag, TRACE_FREQUENCY);

				if (atoll (tmp) > 0)
					num_sampling_hwc++;
			}
			XML_FREE(enabled);
		}
		set_tag = set_tag->next;
	}

	if (num_sampling_hwc > 0)
	{
		t_counters = (char **) malloc (sizeof(char*) * num_sampling_hwc);
		t_periods = (unsigned long long *) malloc (sizeof(unsigned long long) * num_sampling_hwc);
	
		/* Parse all HWC sets, and annotate them to use them later */
		set_tag = current->xmlChildrenNode;
		i = 0;
		while (set_tag != NULL && i < num_sampling_hwc)
		{
			/* Skip coments */
			if (!xmlStrcasecmp (set_tag->name, xmlTEXT) || !xmlStrcmp (set_tag->name, xmlCOMMENT))
			{
			}
			else if (!xmlStrcasecmp (set_tag->name, TRACE_SAMPLING))
			{
				enabled = xmlGetProp_env (rank, set_tag, TRACE_ENABLED);
				if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
				{
					char *tmp = (char*) xmlGetProp_env (rank, set_tag, TRACE_PERIOD);
					if (tmp == NULL)
						tmp = (char*) xmlGetProp_env (rank, set_tag, TRACE_FREQUENCY);

					t_counters[i] = (char*) xmlNodeListGetString_env (rank, xmldoc, set_tag->xmlChildrenNode, 1);
					t_periods[i] = getFactorValue (tmp, "XML:: sampling <period property> (or <frequency>)", rank);

					if (t_periods[i] <= 0)
					{
						mfprintf (stderr, PACKAGE_NAME": Error invalid sampling period for counter %s\n", t_counters[i]);
					}
					else
						i++;
				}
				XML_FREE(enabled);
			}
			set_tag = set_tag->next;
		}
	}
	
	*num = num_sampling_hwc;
	*periods = t_periods;
	*counters = t_counters;
}

/* Configure CPU HWC information */
static void Parse_XML_Counters_CPU (int rank, xmlDocPtr xmldoc, xmlNodePtr current)
{
	xmlNodePtr set_tag;
	char **setofcounters;
	xmlChar *enabled;
	int numofcounters;
	int numofsets = 0;
	int i;

	/* Parse all HWC sets, and annotate them to use them later */
	set_tag = current->xmlChildrenNode;
	while (set_tag != NULL)
	{
		/* Skip coments */
		if (!xmlStrcasecmp (set_tag->name, xmlTEXT) || !xmlStrcmp (set_tag->name, xmlCOMMENT))
		{
		}
		else if (!xmlStrcasecmp (set_tag->name, TRACE_HWCSET))
		{
			/* This 'numofsets' is the pretended number of set in the XML line. 
			It will help debugging the XML when multiple sets are defined */
			numofsets++;

			enabled = xmlGetProp_env (rank, set_tag, TRACE_ENABLED);
			if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
			{
				int OvfNum;
				char **OvfCounters;
				unsigned long long *OvfPeriods;

				char *counters, *domain, *changeat_glops, *changeat_time;
				
				counters = (char*) xmlNodeListGetString_env (rank, xmldoc, set_tag->xmlChildrenNode, 1);
				domain = (char*) xmlGetProp_env (rank, set_tag, TRACE_HWCSET_DOMAIN);
				changeat_glops = (char*) xmlGetProp_env (rank, set_tag, TRACE_HWCSET_CHANGEAT_GLOBALOPS);
				changeat_time = (char*) xmlGetProp_env (rank, set_tag, TRACE_HWCSET_CHANGEAT_TIME);

				numofcounters = explode (counters, ",", &setofcounters);

				Parse_XML_Counters_CPU_Sampling (rank, xmldoc, set_tag, &OvfNum, &OvfCounters, &OvfPeriods);

				HWC_Add_Set (numofsets, rank, numofcounters, setofcounters, domain, changeat_glops, changeat_time, OvfNum, OvfCounters, OvfPeriods);

				for (i = 0; i < numofcounters; i++)
					xfree (setofcounters[i]);

				XML_FREE(counters);
				XML_FREE(changeat_glops);
				XML_FREE(changeat_time);
				XML_FREE(domain);
			}
			XML_FREE(enabled);
		}
		set_tag = set_tag->next;
	}
}
#endif /* USE_HARDWARE_COUNTERS */

/* Configure Counters related parameters */
static void Parse_XML_Counters (int rank, int world_size, xmlDocPtr xmldoc, xmlNodePtr current_tag)
{
	xmlNodePtr tag;

	UNREFERENCED_PARAMETER(xmldoc);
#if !USE_HARDWARE_COUNTERS
	UNREFERENCED_PARAMETER(world_size);
#endif

	/* Parse all TAGs, and annotate them to use them later */
	tag = current_tag->xmlChildrenNode;
	while (tag != NULL)
	{
		/* Here we will check all the options for <counters tag>. Nowadays the only
		   available subtag is <cpu> which depends on the availability of PAPI. */
		/* Skip coments */
		if (!xmlStrcasecmp (tag->name, xmlTEXT) || !xmlStrcmp (tag->name, xmlCOMMENT))
		{
		}
		/* Check if the HWC are being configured at the XML. If so, init them
	   and gather all the sets so as to usem them later. */
		else if (!xmlStrcasecmp (tag->name, TRACE_CPU))
		{
			xmlChar *hwc_enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			char *hwc_startset = (char*) xmlGetProp_env (rank, tag, TRACE_STARTSET);
			if (hwc_enabled != NULL && !xmlStrcasecmp(hwc_enabled, xmlYES))
			{
#if USE_HARDWARE_COUNTERS
				HWC_Initialize (0);

				Parse_XML_Counters_CPU (rank, xmldoc, tag);
				if (hwc_startset != NULL)
					HWC_Parse_XML_Config (rank, world_size, hwc_startset);
#else
				mfprintf (stdout, PACKAGE_NAME": <%s> tag at <%s> level will be ignored. This library does not support CPU HW.\n", TRACE_CPU, TRACE_COUNTERS);
#endif
			}
			XML_FREE(hwc_startset);
			XML_FREE(hwc_enabled);
		}
		else if (!xmlStrcasecmp (tag->name, TRACE_NETWORK))
		{
#if defined(TEMPORARILY_DISABLED)
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			tracejant_network_hwc = (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES));
			mfprintf (stdout, PACKAGE_NAME": Network counters are %s.\n", tracejant_network_hwc?"enabled":"disabled");
			XML_FREE(enabled);
#endif
		}
		else if (!xmlStrcasecmp (tag->name, TRACE_RUSAGE))
		{
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			tracejant_rusage = (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES));
			mfprintf (stdout, PACKAGE_NAME": Resource usage is %s at flush buffer.\n", tracejant_rusage?"enabled":"disabled");
			XML_FREE(enabled);
		}
		else if (!xmlStrcasecmp (tag->name, TRACE_MEMUSAGE))
		{
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			tracejant_memusage = (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES));
			mfprintf (stdout, PACKAGE_NAME": Memory usage is %s at flush buffer.\n", tracejant_memusage?"enabled":"disabled");
			XML_FREE(enabled);
		}
		else
		{
			mfprintf (stderr, PACKAGE_NAME": XML unknown tag '%s' at <Counters> level\n", tag->name);
		}

		tag = tag->next;
	}
}

#if defined(HAVE_ONLINE)
static void Parse_XML_Online (int rank, xmlDocPtr xmldoc, xmlNodePtr current_tag)
{
  xmlNodePtr tag;

  tag = current_tag;
  xmlChar *analysis  = xmlGetProp_env (rank, tag, RC_ONLINE_ANALYSIS_TYPE);
  xmlChar *frequency = xmlGetProp_env (rank, tag, RC_ONLINE_ANALYSIS_FREQ);

  /* Configure the type of analysis */
  if (analysis != NULL)
  {
    if (xmlStrcasecmp(analysis, RC_ONLINE_CLUSTERING) == 0)
    {
      Online_SetAnalysis(ONLINE_DO_CLUSTERING);
    }
    else if (xmlStrcasecmp(analysis, RC_ONLINE_SPECTRAL) == 0)
    {
      Online_SetAnalysis(ONLINE_DO_SPECTRAL);
    }
    else
    {
      mfprintf(stderr, PACKAGE_NAME": XML Error: Value '%s' is not valid for property '<%s>'%s'\n",
        analysis, REMOTE_CONTROL_METHOD_ONLINE, RC_ONLINE_ANALYSIS_TYPE);
      exit(-1);
    }
  }

  /* Configure the frequency of the analysis */
  if (frequency != NULL)
  {
    Online_SetFrequency(atoi(frequency));
  }

  XML_FREE(analysis);
  XML_FREE(frequency);

  tag = current_tag->xmlChildrenNode;
  while (tag != NULL)
  {
    if (!xmlStrcasecmp (tag->name, xmlTEXT) || !xmlStrcmp (tag->name, xmlCOMMENT)) { /* Skip coments */ }
#if defined(HAVE_CLUSTERING)
    else if (!xmlStrcasecmp (tag->name, RC_ONLINE_CLUSTERING))
    {

    }
#endif
#if defined(HAVE_SPECTRAL)
    else if (!xmlStrcasecmp (tag->name, RC_ONLINE_SPECTRAL))
    {
    }
#endif
    tag = tag->next;
  }
}
#endif

/* Configure <remote-control> related parameters */
static void Parse_XML_RemoteControl (int rank, xmlDocPtr xmldoc, xmlNodePtr current_tag)
{
  xmlNodePtr tag;
  int ActiveRemoteControls = 0;

#if !defined(HAVE_ONLINE)
	UNREFERENCED_PARAMETER(xmldoc);
#endif

  /* Parse all TAGs, and annotate them to use them later */
  tag = current_tag->xmlChildrenNode;
  while (tag != NULL)
  {
    if (!xmlStrcasecmp (tag->name, xmlTEXT) || !xmlStrcmp (tag->name, xmlCOMMENT)) { /* Skip comments */ }

    else if (!xmlStrcasecmp (tag->name, REMOTE_CONTROL_METHOD_ONLINE))
    {
      xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
      if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
      {
#if defined(HAVE_ONLINE)
        ActiveRemoteControls++;

        /* Enable the on-line analysis */
        Online_Enable();

        /* Parse the on-line analysis configuration */
        Parse_XML_Online(rank, xmldoc, current_tag);
#else
        mfprintf(stdout, PACKAGE_NAME": XML Warning: Remote control mechanism set to \"On-line analysis\" but this library does not support it! Setting will be ignored...\n");
#endif
      }
      XML_FREE(enabled);
    }
    else if (!xmlStrcasecmp (tag->name, REMOTE_CONTROL_METHOD_SIGNAL))
    {
      xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
      if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
      {
        ActiveRemoteControls++;

        /* Which SIGNAL will we use to interrupt the tracing */
        xmlChar *which = xmlGetProp_env (rank, tag,  RC_SIGNAL_WHICH);
        if (which != NULL)
        {
          if ((xmlStrcasecmp (which, (xmlChar*) "USR1") == 0) || (xmlStrcmp (which, (xmlChar*) "") == 0))
          {
            mfprintf (stdout, PACKAGE_NAME": Signal USR1 will flush buffers to disk and stop further tracing\n");
            Signals_SetupFlushAndTerminate (SIGUSR1);
          }
          else if (xmlStrcasecmp (which, (xmlChar *) "USR2") == 0)
          {
            mfprintf (stdout, PACKAGE_NAME": Signal USR2 will flush buffers to disk and stop further tracing\n");
            Signals_SetupFlushAndTerminate (SIGUSR2);
          }
          else
          {
            mfprintf (stderr, PACKAGE_NAME": XML Error: Value '%s' is not valid for property '<%s>%s'\n", 
            which, REMOTE_CONTROL_METHOD_SIGNAL, RC_SIGNAL_WHICH);
          }
        }
        XML_FREE(which);
      }
      XML_FREE(enabled);
    }
    tag = tag->next;
  }
  if (ActiveRemoteControls > 1)
  {
    mfprintf (stderr, PACKAGE_NAME": XML error: Only 1 remote control mechanism can be activated at <%s>\n", TRACE_REMOTE_CONTROL);
    exit(-1);
  }
}

/* Configure <others> related parameters */
static void Parse_XML_TraceControl (int rank, int world_size, xmlDocPtr xmldoc, xmlNodePtr current_tag)
{
	xmlNodePtr tag;

	UNREFERENCED_PARAMETER(world_size);

	/* Parse all TAGs, and annotate them to use them later */
	tag = current_tag->xmlChildrenNode;
	while (tag != NULL)
	{
		if (!xmlStrcasecmp (tag->name, xmlTEXT) || !xmlStrcmp (tag->name, xmlCOMMENT)) { /* Skip comments */ }

		/* Must we check for a control file? */
		else if (!xmlStrcasecmp (tag->name, TRACE_CONTROL_FILE))
		{
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
			{
				char *c_file = (char*) xmlNodeListGetString_env (rank, xmldoc, tag->xmlChildrenNode, 1);
				if (c_file != NULL)
				{
					char *tmp;

					CheckForControlFile = TRUE;
					strcpy (ControlFileName, c_file);
					mfprintf (stdout, PACKAGE_NAME": Control file is '%s'. Tracing will be disabled until the file exists.\n", c_file);

					/* Let the user tune how often will be checked the existence of the control file */
					tmp = (char*) xmlGetProp_env (rank, tag, TRACE_FREQUENCY);
					if (tmp != NULL)
					{
						WantedCheckControlPeriod = getTimeFromStr (tmp, TRACE_FREQUENCY, rank);
						if (WantedCheckControlPeriod >= 1000000000)
						{
							mfprintf (stdout, PACKAGE_NAME": Control file will be checked every %llu seconds\n", WantedCheckControlPeriod / 1000000000);
						}
						else if (WantedCheckControlPeriod < 1000000000 && WantedCheckControlPeriod > 0)
						{
							mfprintf (stdout, PACKAGE_NAME": Control file will be checked every %llu nanoseconds\n", WantedCheckControlPeriod);
						}
					}
					XML_FREE(tmp);
				}
				XML_FREE(c_file);
			}
			XML_FREE(enabled);
		}
		/* Must we check for global-ops counters? */
		else if (!xmlStrcasecmp (tag->name, TRACE_CONTROL_GLOPS))
		{
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
			{
#if defined(MPI_SUPPORT)
				char *trace_intervals = (char*) xmlNodeListGetString_env (rank, xmldoc, tag->xmlChildrenNode, 1);
				if (trace_intervals != NULL)
				{
					CheckForGlobalOpsTracingIntervals = TRUE;
					Parse_GlobalOps_Tracing_Intervals (trace_intervals);
					XML_FREE(trace_intervals);
				}
#else
				mfprintf (stdout, PACKAGE_NAME": Warning! <%s> tag will be ignored. This library does not support MPI.\n", TRACE_CONTROL_GLOPS);
#endif
			}
			XML_FREE(enabled);
		}

		else if (!xmlStrcasecmp (tag->name, TRACE_REMOTE_CONTROL))
		{
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
			{
				Parse_XML_RemoteControl (rank, xmldoc, tag);
			}
			XML_FREE(enabled);
		}

		else
		{
			mfprintf (stderr, PACKAGE_NAME": XML unknown tag '%s' at <%s> level\n", tag->name, TRACE_CONTROL);
		}

		tag = tag->next;
	}
}

#if defined(EMBED_MERGE_IN_TRACE)

#include "options.h" /* for merger options */

/* Configure <merge> related parameters */
static void Parse_XML_Merge (int rank, xmlDocPtr xmldoc, xmlNodePtr current_tag,
	xmlChar *tracetype)
{
	xmlChar *synchronization;
	xmlChar *binary;
#if defined(MPI_SUPPORT)
	xmlChar *treefanout;
#endif
	xmlChar *maxmemory;
	xmlChar *jointstates;
	xmlChar *sortaddresses;
	xmlChar *keepmpits;
	xmlChar *traceoverwrite;
	char *filename;

	if (tracetype != NULL && !xmlStrcasecmp (tracetype, TRACE_TYPE_DIMEMAS))
		set_option_merge_ParaverFormat (FALSE);
	else
		set_option_merge_ParaverFormat (TRUE);

	keepmpits = xmlGetProp_env (rank, current_tag, TRACE_MERGE_KEEP_MPITS);
	if (keepmpits != NULL)
		set_option_merge_RemoveFiles (!(!xmlStrcasecmp (keepmpits, xmlYES)));
	else
		set_option_merge_RemoveFiles (FALSE);

	traceoverwrite = xmlGetProp_env (rank, current_tag, TRACE_MERGE_OVERWRITE);
	if (traceoverwrite != NULL)
		set_option_merge_TraceOverwrite (!xmlStrcasecmp (traceoverwrite, xmlYES));
	else
		set_option_merge_TraceOverwrite (TRUE);

	sortaddresses = xmlGetProp_env (rank, current_tag, TRACE_MERGE_SORTADDRESSES);
	if (sortaddresses != NULL)
		set_option_merge_SortAddresses (!xmlStrcasecmp (sortaddresses, xmlYES));
	else
		set_option_merge_SortAddresses (FALSE);

	synchronization = xmlGetProp_env (rank, current_tag, TRACE_MERGE_SYNCHRONIZATION);
	if (synchronization != NULL && !xmlStrcasecmp (synchronization, TRACE_MERGE_SYN_DEFAULT))
	{
		set_option_merge_SincronitzaTasks (TRUE);
		set_option_merge_SincronitzaTasks_byNode (TRUE);
	}
	else if (synchronization != NULL && !xmlStrcasecmp (synchronization, TRACE_MERGE_SYN_NODE))
	{
		set_option_merge_SincronitzaTasks (TRUE);
		set_option_merge_SincronitzaTasks_byNode (TRUE);
	}
	else if (synchronization != NULL && !xmlStrcasecmp (synchronization, TRACE_MERGE_SYN_TASK))
	{
		set_option_merge_SincronitzaTasks (TRUE);
		set_option_merge_SincronitzaTasks_byNode (FALSE);
	}
	else if (synchronization != NULL && !xmlStrcasecmp (synchronization, xmlNO))
	{
		set_option_merge_SincronitzaTasks (FALSE);
		set_option_merge_SincronitzaTasks_byNode (FALSE);
	}

	maxmemory = xmlGetProp_env (rank, current_tag, TRACE_MERGE_MAX_MEMORY);
	if (maxmemory != NULL)
	{
		if (atoi(maxmemory) <= 0)
		{
			mfprintf (stderr, PACKAGE_NAME": Warning! Invalid value '%s' for property <%s> in tag <%s>. Setting to 512Mbytes.\n",
				maxmemory, TRACE_MERGE, TRACE_MERGE_MAX_MEMORY);
			set_option_merge_MaxMem (16);
		}
		else if (atoi(maxmemory) <= 16)
		{
			mfprintf (stderr, PACKAGE_NAME": Warning! Low value '%s' for property <%s> in tag <%s>. Setting to 16Mbytes.\n",
				maxmemory, TRACE_MERGE, TRACE_MERGE_MAX_MEMORY);
			set_option_merge_MaxMem (16);
		}
		else
		{
			set_option_merge_MaxMem (atoi(maxmemory));
		}
	}

#if defined(MPI_SUPPORT)
	treefanout = xmlGetProp_env (rank, current_tag, TRACE_MERGE_TREE_FAN_OUT);
	if (treefanout != NULL)
	{
		if (atoi(treefanout) > 1)
		{
			set_option_merge_TreeFanOut (atoi(treefanout));
		}
		else
		{
			mfprintf (stderr, PACKAGE_NAME": Warning! Invalid value '%s' for property <%s> in tag <%s>.\n",
				treefanout, TRACE_MERGE, TRACE_MERGE_TREE_FAN_OUT);
		}
	}
#endif

	binary = xmlGetProp_env (rank, current_tag, TRACE_MERGE_BINARY);
	if (binary != NULL)	
		set_merge_ExecutableFileName (binary);

	jointstates = xmlGetProp_env (rank, current_tag, TRACE_MERGE_JOINT_STATES);
	if (jointstates != NULL && !xmlStrcasecmp (jointstates, xmlNO))
		set_option_merge_JointStates (FALSE);
	else
		set_option_merge_JointStates (TRUE);


	filename = xmlNodeListGetString_env (rank, xmldoc, current_tag->xmlChildrenNode, 1);
	if (filename == NULL || strlen(filename) == 0)
	{
		if (get_option_merge_ParaverFormat())
			set_merge_OutputTraceName (DEFAULT_PRV_OUTPUT_NAME);
		else
			set_merge_OutputTraceName (DEFAULT_DIM_OUTPUT_NAME);
	}
	else
	{
		set_merge_OutputTraceName (filename);
	}

	XML_FREE (synchronization);
	XML_FREE (sortaddresses);
	XML_FREE (binary);
#if defined(MPI_SUPPORT)
	XML_FREE (treefanout);
#endif
	XML_FREE (maxmemory);
	XML_FREE (jointstates);
	XML_FREE (keepmpits);
	XML_FREE (traceoverwrite);
}
#endif

/* Configure <others> related parameters */
static void Parse_XML_Others (int rank, xmlDocPtr xmldoc, xmlNodePtr current_tag)
{
	xmlNodePtr tag;

	/* Parse all TAGs, and annotate them to use them later */
	tag = current_tag->xmlChildrenNode;
	while (tag != NULL)
	{
		/* Skip coments */
		if (!xmlStrcasecmp (tag->name, xmlTEXT) || !xmlStrcmp (tag->name, xmlCOMMENT))
		{
		}
		/* Must the trace run for at least some time? */
		else if (!xmlStrcasecmp (tag->name, TRACE_MINIMUM_TIME))
		{
			xmlChar *enabled = xmlGetProp_env (rank, tag, TRACE_ENABLED);
			if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
			{
				char *str = (char*) xmlNodeListGetString_env (rank, xmldoc, tag->xmlChildrenNode, 1);
				if (str != NULL)
				{
					MinimumTracingTime = getTimeFromStr (str, TRACE_MINIMUM_TIME, rank);
					hasMinimumTracingTime = ( MinimumTracingTime != 0);
					if (MinimumTracingTime >= 1000000000)
					{
						mfprintf (stdout, PACKAGE_NAME": Minimum tracing time will be %llu seconds\n", MinimumTracingTime / 1000000000);
					}
					else if (MinimumTracingTime < 1000000000 && MinimumTracingTime > 0)
					{
						mfprintf (stdout, PACKAGE_NAME": Minimum tracing time will be %llu nanoseconds\n", MinimumTracingTime);
					}
				}
				XML_FREE(str);
			}
			XML_FREE(enabled);
		}
		else
		{
			mfprintf (stderr, PACKAGE_NAME": XML unknown tag '%s' at <Others> level\n", tag->name);
		}

		tag = tag->next;
	}
}

void Parse_XML_File (int rank, int world_size, char *filename)
{
	xmlNodePtr current_tag;
	xmlDocPtr  xmldoc;
	xmlNodePtr root_tag;
	char cwd[TMP_DIR];

	/*
	* This initialize the library and check potential ABI mismatches
	* between the version it was compiled for and the actual shared
	* library used.
	*/
	LIBXML_TEST_VERSION;

	mfprintf (stdout, PACKAGE_NAME": Parsing the configuration file (%s) begins\n", filename);   
	xmldoc = xmlParseFile (filename);
	if (xmldoc != NULL)
	{
		root_tag = xmlDocGetRootElement (xmldoc);
		if (root_tag != NULL)
		{
			if (xmlStrcasecmp(root_tag->name, TRACE_TAG))
			{	
				mfprintf (stderr, PACKAGE_NAME": Invalid configuration file\n");
			}
			else
			{
				/*
					 Are the MPI & OpenMP tracing enabled? Is the tracing fully enabled? 
					 try to remember if these values have been changed previously by the
					 API routines.
				*/

				/* Full tracing control */
				char *tracehome = (char*) xmlGetProp_env (rank, root_tag, TRACE_HOME);
				xmlChar *xmlparserid = xmlGetProp_env (rank,root_tag, TRACE_PARSER_ID);
				xmlChar *traceenabled = xmlGetProp_env (rank, root_tag, TRACE_ENABLED);
				xmlChar *traceinitialmode = xmlGetProp_env (rank, root_tag, TRACE_INITIAL_MODE);
				xmlChar *tracetype = xmlGetProp_env (rank, root_tag, TRACE_TYPE);
				mpitrace_on = (traceenabled != NULL) && !xmlStrcasecmp (traceenabled, xmlYES);

				if (!mpitrace_on)
				{
					mfprintf (stdout, PACKAGE_NAME": Application has been linked or preloaded with mpitrace, BUT tracing is NOT set!\n");
				}
				else
				{
					/* Where is the tracing located? If defined, copy to the correct buffer! */
					if (xmlStrcasecmp (&rcsid[1], xmlparserid)) /* Skip first $ char */
					{
						mfprintf (stderr, PACKAGE_NAME": WARNING!\n");
						mfprintf (stderr, PACKAGE_NAME": WARNING! XML parser version and property '%s' do not match. Check the XML file. Trying to proceed...\n", TRACE_PARSER_ID);
						mfprintf (stderr, PACKAGE_NAME": WARNING!\n");
						mfprintf (stderr, PACKAGE_NAME": %s found '%s' when expecting '%s'.\n", TRACE_PARSER_ID, xmlparserid, &rcsid[1]);
					}

					if (tracehome != NULL)
					{
						strncpy (trace_home, tracehome, TMP_DIR);
						mfprintf (stdout, PACKAGE_NAME": Tracing package is located on %s\n", trace_home);
					}
					else
					{
						mfprintf (stdout, PACKAGE_NAME": Warning! <%s> tag has no <%s> property defined.\n", TRACE_TAG, TRACE_HOME);
					}

					if (traceinitialmode != NULL)
					{
						if (!xmlStrcasecmp (traceinitialmode, TRACE_INITIAL_MODE_DETAIL))
						{
							TMODE_setInitial (TRACE_MODE_DETAIL);
						}
						else if (!xmlStrcasecmp (traceinitialmode, TRACE_INITIAL_MODE_BURSTS))
						{
							TMODE_setInitial (TRACE_MODE_BURSTS);
						}
						else
						{
							mfprintf (stdout, PACKAGE_NAME": Warning! Invalid value '%s' for property <%s> in tag <%s>.\n", traceinitialmode, TRACE_INITIAL_MODE, TRACE_TAG);
							TMODE_setInitial (TRACE_MODE_DETAIL);
						}
					}
					else
					{
						mfprintf (stdout, PACKAGE_NAME": Warning! Not given value for property <%s> in tag <%s>.\n", TRACE_INITIAL_MODE, TRACE_TAG);
						TMODE_setInitial (TRACE_MODE_DETAIL);
					}

					if (tracetype != NULL)
					{
						if (!xmlStrcasecmp (tracetype, TRACE_TYPE_PARAVER))
						{
							mfprintf (stdout, PACKAGE_NAME": Generating intermediate files for Paraver traces.\n");
							Clock_setType (REAL_CLOCK);
						}
						else if (!xmlStrcasecmp (tracetype, TRACE_TYPE_DIMEMAS))
						{
							mfprintf (stdout, PACKAGE_NAME": Generating intermediate files for Dimemas traces.\n");
							Clock_setType (USER_CLOCK);
						}
						else
						{
							mfprintf (stdout, PACKAGE_NAME": Warning! Invalid value '%s' for property <%s> in tag <%s>.\n", tracetype, TRACE_TYPE, TRACE_TAG);
							Clock_setType (REAL_CLOCK);
						}
					}
					else
					{
						mfprintf (stdout, PACKAGE_NAME": Warning! Not given value for property <%s> in tag <%s>.\n", TRACE_TYPE, TRACE_TAG);
						Clock_setType (REAL_CLOCK);
					}
				}
				XML_FREE(xmlparserid);
				XML_FREE(tracetype);
				XML_FREE(traceinitialmode);
				XML_FREE(traceenabled);
				XML_FREE(tracehome);

				current_tag = root_tag->xmlChildrenNode;
				while (current_tag != NULL && mpitrace_on)
				{
					/* Skip coments */
					if (!xmlStrcasecmp (current_tag->name, xmlTEXT) || !xmlStrcmp (current_tag->name, xmlCOMMENT))
					{
					}
					/* UF related information instrumentation */
					else if (!xmlStrcasecmp (current_tag->name, TRACE_USERFUNCTION))
					{
						xmlChar *enabled = xmlGetProp_env (rank, current_tag, TRACE_ENABLED);
						if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
							Parse_XML_UF (rank, current_tag);
						XML_FREE(enabled);
					}
					/* Callers related information instrumentation */
					else if (!xmlStrcasecmp (current_tag->name, TRACE_CALLERS))
					{
						xmlChar *enabled = xmlGetProp_env (rank, current_tag, TRACE_ENABLED);
						if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
							Parse_XML_Callers (rank, xmldoc, current_tag);
						XML_FREE(enabled);
					}
					/* CUDA related configuration */
					else if (!xmlStrcasecmp (current_tag->name, TRACE_CUDA))
					{
						xmlChar *enabled = xmlGetProp_env (rank, current_tag, TRACE_ENABLED);

						if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
						{
#if defined(CUDA_SUPPORT)
							tracing_cuda = TRUE;
							Parse_XML_CUDA (rank, xmldoc, current_tag);
#else
							mfprintf (stdout, PACKAGE_NAME": Warning! <%s> tag will be ignored. This library does not support CUDA.\n", TRACE_CUDA);
#endif
						}
						else if (enabled != NULL && !xmlStrcasecmp (enabled, xmlNO))
							tracing_cuda = FALSE;
						XML_FREE(enabled);
					}
					/* OpenCL related configuration */
					else if (!xmlStrcasecmp (current_tag->name, TRACE_OPENCL))
					{
						xmlChar *enabled = xmlGetProp_env (rank, current_tag, TRACE_ENABLED);

						if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
						{
#if defined(OPENCL_SUPPORT)
							tracing_opencl = TRUE;
							Parse_XML_OpenCL (rank, xmldoc, current_tag);
#else
							mfprintf (stdout, PACKAGE_NAME": Warning! <%s> tag will be ignored. This library does not support OpenCL.\n", TRACE_OPENCL);
#endif
						}
						else if (enabled != NULL && !xmlStrcasecmp (enabled, xmlNO))
							tracing_opencl = FALSE;
						XML_FREE(enabled);
					}
					/* MPI related configuration */
					else if (!xmlStrcasecmp (current_tag->name, TRACE_MPI))
					{
						xmlChar *enabled = xmlGetProp_env (rank, current_tag, TRACE_ENABLED);

						if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
						{
#if defined(MPI_SUPPORT)
							tracejant_mpi = TRUE;
							Parse_XML_MPI (rank, xmldoc, current_tag);
#else
							mfprintf (stdout, PACKAGE_NAME": Warning! <%s> tag will be ignored. This library does not support MPI.\n", TRACE_MPI);
#endif
						}
						else if (enabled != NULL && !xmlStrcasecmp (enabled, xmlNO))
							tracejant_mpi = FALSE;
						XML_FREE(enabled);
					}
					/* PACX related configuration */
					else if (!xmlStrcasecmp (current_tag->name, TRACE_PACX))
					{
						xmlChar *enabled = xmlGetProp_env (rank, current_tag, TRACE_ENABLED);
						if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
						{
#if defined(PACX_SUPPORT)
							tracejant_pacx = TRUE;
							Parse_XML_PACX (rank, xmldoc, current_tag);
#else
							mfprintf (stdout, PACKAGE_NAME": Warning! <%s> tag will be ignored. This library does not support PACX.\n", TRACE_PACX);
#endif
						}
						else if (enabled != NULL && !xmlStrcasecmp (enabled, xmlNO))
							tracejant_pacx = FALSE;
						XML_FREE(enabled);
					}
					/* Bursts related configuration */
					else if (!xmlStrcasecmp (current_tag->name, TRACE_BURSTS))
					{
						xmlChar *enabled = xmlGetProp_env (rank, current_tag, TRACE_ENABLED);
						if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
						{
							Parse_XML_Bursts (rank, xmldoc, current_tag);
						}
						XML_FREE(enabled);
					}
					/* OpenMP related configuration */
					else if (!xmlStrcasecmp (current_tag->name, TRACE_OMP))
					{
						xmlChar *enabled = xmlGetProp_env (rank, current_tag, TRACE_ENABLED);
						if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
						{
#if defined(OMP_SUPPORT) || defined(SMPSS_SUPPORT)
							tracejant_omp = TRUE;
							Parse_XML_OMP (rank, xmldoc, current_tag);
#else
							mfprintf (stdout, PACKAGE_NAME": Warning! <%s> tag will be ignored. This library does not support OpenMP.\n", TRACE_OMP);
							tracejant_omp = FALSE;
#endif
						}
						else
							tracejant_omp = FALSE;
						XML_FREE(enabled);
					}
					/* pthread related configuration */
					else if (!xmlStrcasecmp (current_tag->name, TRACE_PTHREAD))
					{
						xmlChar *enabled = xmlGetProp_env (rank, current_tag, TRACE_ENABLED);
						if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
						{
#if defined(PTHREAD_SUPPORT)
							tracejant_pthread = TRUE;
							Parse_XML_PTHREAD (rank, xmldoc, current_tag);
#else
							mfprintf (stdout, PACKAGE_NAME": Warning! <%s> tag will be ignored. This library does not support pthread.\n", TRACE_OMP);
							tracejant_pthread = FALSE;
#endif
						}
						else
							tracejant_pthread = FALSE;
						XML_FREE(enabled);
					}
					/* time-based sampling configuration */
					else if (!xmlStrcasecmp (current_tag->name, TRACE_SAMPLING))
					{
						xmlChar *enabled = xmlGetProp_env (rank, current_tag, TRACE_ENABLED);
						if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
						{
#if defined(SAMPLING_SUPPORT)
							Parse_XML_Sampling (rank, current_tag);
#else
							mfprintf (stdout, PACKAGE_NAME": Warning! <%s> tag will be ignored. This library does not support sampling.\n", TRACE_SAMPLING);
#endif
						}
						XML_FREE(enabled);
					}
					/* SPU related configuration*/
					else if (!xmlStrcasecmp (current_tag->name, TRACE_CELL))
					{
						xmlChar *enabled = xmlGetProp_env (rank, current_tag, TRACE_ENABLED);
						if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
						{
#if defined(IS_CELL_MACHINE)
							cell_tracing_enabled = TRUE;
#if 0 /* unimplemented right now */
							Parse_XML_SPU (rank, xmldoc, current_tag);
#endif
#else
							mfprintf (stdout, PACKAGE_NAME": Warning! <%s> tag will be ignored. This library does not support Cell BE processors.\n", TRACE_CELL);
#endif
						}
#if defined(IS_CELL_MACHINE)
						else
							cell_tracing_enabled = FALSE;
#endif
						XML_FREE(enabled);
					}
					/* Storage related configuration */
					else if (!xmlStrcasecmp (current_tag->name, TRACE_STORAGE))
					{
						xmlChar *enabled = xmlGetProp_env (rank, current_tag, TRACE_ENABLED);
						if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
							Parse_XML_Storage (rank, xmldoc, current_tag);
						XML_FREE(enabled);
					}
					/* Buffering related configuration */
					else if (!xmlStrcasecmp (current_tag->name, TRACE_BUFFER))
					{
						xmlChar *enabled = xmlGetProp_env (rank, current_tag, TRACE_ENABLED);
						if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
							Parse_XML_Buffer (rank, xmldoc, current_tag);
						XML_FREE(enabled);
					}
					/* Check if some other configuration info must be gathered */
					else if (!xmlStrcasecmp (current_tag->name, TRACE_OTHERS))
					{
						xmlChar *enabled = xmlGetProp_env (rank, current_tag, TRACE_ENABLED);
						if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
							Parse_XML_Others (rank, xmldoc, current_tag);
						XML_FREE(enabled);
					}
					/* Check if some kind of counters must be gathered */
					else if (!xmlStrcasecmp (current_tag->name, TRACE_COUNTERS))
					{
						xmlChar *enabled = xmlGetProp_env (rank, current_tag, TRACE_ENABLED);
						if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
							Parse_XML_Counters (rank, world_size, xmldoc, current_tag);
						XML_FREE(enabled);
					}
					/* Check for tracing control */
					else if (!xmlStrcasecmp (current_tag->name, TRACE_CONTROL))
					{
						xmlChar *enabled = xmlGetProp_env (rank, current_tag, TRACE_ENABLED);
						if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
							Parse_XML_TraceControl (rank, world_size, xmldoc, current_tag);
						XML_FREE(enabled);
					}
					/* Check for merging control */
					else if (!xmlStrcasecmp (current_tag->name, TRACE_MERGE))
					{
#if defined(EMBED_MERGE_IN_TRACE)
						xmlChar *enabled = xmlGetProp_env (rank, current_tag, TRACE_ENABLED);
						xmlChar *tracetype = xmlGetProp_env (rank, root_tag, TRACE_TYPE);
						if (enabled != NULL && !xmlStrcasecmp (enabled, xmlYES))
						{
							Parse_XML_Merge (rank, xmldoc, current_tag, tracetype);
							MergeAfterTracing = TRUE;
						}
						else
							MergeAfterTracing = FALSE;
						XML_FREE(tracetype);
						XML_FREE(enabled);
#else
						mfprintf (stdout, PACKAGE_NAME": Warning! <%s> tag will be ignored. This library does not have merging process embedded.\n", TRACE_MERGE);
#endif
					}
					else
					{
						mfprintf (stderr, PACKAGE_NAME": Warning! XML unknown tag '%s'\n", current_tag->name);
					}

					current_tag = current_tag->next;
				}
			}
		}
		else
		{
			mfprintf (stderr, PACKAGE_NAME": Error! Empty mpitrace configuration file\n");
		}

		xmlFreeDoc (xmldoc);
	}
	else
	{
		mfprintf (stderr, PACKAGE_NAME": Error! xmlParseFile routine failed!\n");
	}
	mfprintf (stdout, PACKAGE_NAME": Parsing the configuration file (%s) has ended\n", filename);   

	/* If the tracing has been enabled, continue with the configuration */
	if (mpitrace_on)
	{
		char *res_cwd;

		/* Have the user proposed any program name? */
		if (!TracePrefixFound)
			strncpy (PROGRAM_NAME, "TRACE", strlen("TRACE")+1);

		/* Just gather now the variables related to the directories! */

		/* Start obtaining the current directory */
		res_cwd = getcwd (cwd, sizeof(cwd));

		/* Temporal directory must be checked against the configuration of the XML,
    	  and the current directory */
		if (temporal_d == NULL)
			if ((temporal_d = res_cwd) == NULL)
				temporal_d = ".";
		strcpy (tmp_dir, temporal_d);
		/* Force mkdir */
		mkdir_recursive (tmp_dir);

		/* Final directory must be checked against the configuration of the XML, 
  	    the temporal_directory and, finally, the current directory */
		if (final_d == NULL)
			final_d = temporal_d;

		if (strlen(final_d) > 0)
		{
			if (final_d[0] != '/')
				sprintf (final_dir, "%s/%s", res_cwd, final_d);
			else
				strcpy (final_dir, final_d);
		}
		else
			strcpy (final_dir, final_d);

		/* Force mkdir */
		mkdir_recursive (final_dir);

		if (strcmp (final_dir, tmp_dir) != 0)
		{
			mfprintf (stdout, PACKAGE_NAME": Temporal directory for the intermediate traces is %s\n", tmp_dir);
			mfprintf (stdout, PACKAGE_NAME": Final directory for the intermediate traces is %s\n", final_dir);
		}
		else
		{
			mfprintf (stdout, PACKAGE_NAME": Intermediate traces will be stored in %s\n", tmp_dir);
		}
	}

	/* Clean Up the memory allocated by LIBXML_TEST_VERSION */
	xmlCleanupParser();
}

#endif /* HAVE_XML2 */