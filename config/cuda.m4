# AX_CUDA
# --------------------
AC_DEFUN([AX_CUDA],
[
	AC_ARG_WITH(cuda,
		AC_HELP_STRING(
			[--with-cuda@<:@=DIR@:>@],
			[Enable support for tracing CUDA - may be superseded and still necessary by CUPTI if the latter is enabled]
		),
		[cuda_path="${withval}"],
		[cuda_path="none"]
	)

	enable_cuda="no"

	if test "${cuda_path}" != "none" ; then
		AC_MSG_CHECKING([for CUDA])
		if test -d "${cuda_path}" ; then
			if test -x ${cuda_path}/bin/nvcc ; then
				if test -r ${cuda_path}/include/cuda_runtime_api.h ; then
					enable_cuda="yes"
					AC_MSG_RESULT(${cuda_path})
				else
					AC_MSG_ERROR([Cannot find the necessary header files in the CUDA path])
				fi
			else
				AC_MSG_ERROR([Cannot find the CUDA compiler in the given path])
			fi
		else
			AC_MSG_ERROR([The specified path for CUDA does not exist])
		fi
		AX_FLAGS_SAVE()
		AX_FIND_INSTALLATION([CUDA], [${cuda_path}], [cuda])
		AX_FLAGS_RESTORE()
	fi

])

# AX_CUPTI
# --------------------
AC_DEFUN([AX_CUPTI],
[
	AC_REQUIRE([AX_CUDA])

	AC_ARG_WITH(cupti,
		AC_HELP_STRING(
		[--with-cupti@<:@=DIR@:>@],
		[specify where to find CUPTI libraries and includes]
	),
		[cupti_path="${withval}"],
		[cupti_path="none"]
	)

	if test "${cupti_path}" != "none" -a "${enable_cuda}" = "no" ; then
		AC_MSG_ERROR([In order to use --with-cupti, you should pass also --with-cuda])
	fi

	enable_cupti="no"

	if test "${cupti_path}" != "none" ; then
		AC_MSG_CHECKING([for CUPTI])
		if test -d "${cupti_path}" ; then
			if test -r ${cupti_path}/lib/libcupti.so -a -r ${cupti_path}/include/cupti.h -a -r ${cupti_path}/include/cupti_events.h ; then
				AC_MSG_RESULT(${cupti_path})
				enable_cupti="yes"
			else
				AC_MSG_ERROR([Cannot locate CUPTI in the specified directory])
			fi
		else
			AC_MSG_ERROR([The specified path for CUPTI does not exist])
		fi
		AX_FLAGS_SAVE()
		AX_FIND_INSTALLATION([CUPTI], [${cupti_path}], [cupti])
		AX_FLAGS_RESTORE()
	fi

	if test "${enable_cupti}" = "yes" ; then
		AC_DEFINE([CUDA_WITH_CUPTI_INSTRUMENTATION], [1], [Determine if CUDA instrumentation must rely on CUPTI])
	else
		if test "${enable_cuda}" = "yes" ; then
			AC_DEFINE([CUDA_WITHOUT_CUPTI_INSTRUMENTATION], [1], [Determine if CUDA instrumentation must NOT rely on CUPTI])
		fi
	fi

	#
	# CUDA is superseded by CUPTI. If CUPTI is available, use it instead dlopen alternative.
	#
	AM_CONDITIONAL(WANT_CUPTI, test "${enable_cupti}" = "yes" )
	AM_CONDITIONAL(WANT_CUDA, test "${enable_cuda}" = "yes" -a "${enable_cupti}" = "no" )
	AM_CONDITIONAL(WANT_CUDAorCUPTI, test "${enable_cuda}" = "yes" -o "${enable_cupti}" = "yes" )

])

# AX_CUDA_SHOW_CONFIGURATION
# --------------------
AC_DEFUN([AX_CUDA_SHOW_CONFIGURATION],
[
	if test "${enable_cupti}" = "yes" ; then
		echo CUDA instrumentation: yes, through CUPTI
		echo -e \\\tCUDA home : ${cuda_path}
		echo -e \\\tCUPTI home: ${cupti_path}
	else
		if test "${enable_cuda}" = "yes" ; then
			echo CUDA instrumentation: yes, through LD_PRELOAD
			echo -e \\\tCUDA home : ${cuda_path}
		else
			echo CUDA instrumentation: no
		fi
	fi
])