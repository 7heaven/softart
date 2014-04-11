# Requirements:
#  - HEADER_FILES, SOURCE_FILES, RESOURCE_FILES, ADDITIONAL_FILES
#  - ADDITIONAL_INCLUDE_DIRECTORIES, ADDITIONAL_LINK_DIRECTORIES 

IF(CMAKE_GENERATOR MATCHES "Win64")
	SET(SALVIA_ARCH_NAME "x64")
ELSE()
	SET(SALVIA_ARCH_NAME "x86")
ENDIF()

MACRO(SALVIA_CONFIG_SAMPLE_PROJECT ProjectName)

INCLUDE_DIRECTORIES(
	${SALVIA_HOME_DIR}
	${SALVIA_THREAD_POOL_INCLUDE_DIR}
	${SALVIA_BOOST_INCLUDE_DIR}
	${SALVIA_WTL_INCLUDE_DIR}
	${SALVIA_FREEIMAGE_INCLUDE_DIR}
	${ADDITIONAL_INCLUDE_DIRECTORIES}
)

LINK_DIRECTORIES(
	${SALVIA_LIBS}
	${SALVIA_BOOST_LIB_DIR}
	${SALVIA_LLVM_LIB_DIR}
	${SALVIA_FREETYPE_LIB_DIR}
	${SALVIA_FREEIMAGE_LIB_DIR}
	${ADDITIONAL_LINK_DIRECTORIES}
)

if(SALVIA_BUILD_WITH_DIRECTX)
	if(MSVC)
		if(MSVC_VERSION LESS 1700)
			LINK_DIRECTORIES( $ENV{DXSDK_DIR}/Lib/${SALVIA_ARCH_NAME} )
		endif(MSVC_VERSION LESS 1700)
	endif(MSVC)
endif()

SOURCE_GROUP( "Header Files"	FILES ${HEADER_FILES} )
SOURCE_GROUP( "Source Files"	FILES ${SOURCE_FILES} )
SOURCE_GROUP( "Resources" 		FILES ${RESOURCE_FILES} )

ADD_EXECUTABLE( ${ProjectName} ${SOURCE_FILES} ${HEADER_FILES} ${RESOURCE_FILES} ${ADDITIONAL_FILES})

TARGET_LINK_LIBRARIES(${ProjectName} salviau salviax salviar EFLIB ${ADDITIONAL_LIBS} ${ADDITIONAL_LIBS})
SET_TARGET_PROPERTIES(${ProjectName} PROPERTIES FOLDER "Samples")
SALVIA_CONFIG_OUTPUT_PATHS(${ProjectName})
SALVIA_CREATE_VCPROJ_USERFILE(${ProjectName})

ENDMACRO ( SALVIA_CONFIG_SAMPLE_PROJECT )
