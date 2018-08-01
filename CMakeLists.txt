#SET(BUILD_HOME /local/user_data/mkpne_shuchen_shuchen_pltf_lr13l_1_dma/eccm2)


SET( MODULE_NAME
	bigcpm	
)

SET( MODULE_SOURCE_FILES
	${MODULE_NAME}_k.c debug_trace.h bigcpm_ioctl.h
)

SET( MODULE_FILE
	${MODULE_NAME}.ko
)

SET( MODULE_OUTPUT_DIR
	${CMAKE_BINARY_DIR}/modules/${MODULE_NAME}
)

#SET( KERNEL_DIR
#	"/lib/modules/${CMAKE_SYSTEM_VERSION}/build"
#)

#Set( KERNEL_DIR "${BUILD_HOME}/build/linux-eccm2-cgl-build" )
#Set( KERNEL_DIR "${BUILD_HOME}/build/linux-bcem-preempt_rt-build" )

SET( KBUILD_COMMAND
	${CMAKE_MAKE_PROGRAM} -C ${KERNEL_DIR} M=${MODULE_OUTPUT_DIR}
	ARCH=powerpc CROSS_COMPILE="powerpc-wrs-linux-gnu-" 
	modules
)

CONFIGURE_FILE(
	Kbuild.in
	${MODULE_OUTPUT_DIR}/Kbuild
)

FOREACH( MODULE_SOURCE_FILE ${MODULE_SOURCE_FILES} )
	CONFIGURE_FILE(
		${MODULE_SOURCE_FILE}
		${MODULE_OUTPUT_DIR}/${MODULE_SOURCE_FILE}
		COPYONLY
	)
ENDFOREACH( MODULE_SOURCE_FILE )

ADD_CUSTOM_COMMAND(
	OUTPUT ${MODULE_OUTPUT_DIR}/${MODULE_FILE}
	COMMAND ${KBUILD_COMMAND}
	WORKING_DIRECTORY ${MODULE_OUTPUT_DIR}
	DEPENDS ${MODULE_SOURCE_FILES} Kbuild.in
	VERBATIM
)

ADD_CUSTOM_TARGET(
	${MODULE_NAME}
	ALL
	DEPENDS ${MODULE_OUTPUT_DIR}/${MODULE_FILE}
)

#####################
#userspace appl
#
#add_executable(bigcpmtest bigcpm_test.c)
add_executable(bigcpmtest2 bigcpm_test2.c)
add_executable(bigcpmtest3 bigcpm_test3.c)
add_executable(bigcpmtest4 bigcpm_test4.c)



