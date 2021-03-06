SET( PROJECT "qt3d" )
SET( TARGET_NAME snark_graphics_${PROJECT} )
SET( dir ${SOURCE_CODE_BASE_DIR}/graphics/${PROJECT} )

FILE( GLOB source   ${dir}/*.cpp )
FILE( GLOB includes ${dir}/*.h )

QT4_WRAP_CPP( snark_graphics_qt3d_moc ${dir}/view.h OPTIONS -DBOOST_TT_HAS_OPERATOR_HPP_INCLUDED )

SOURCE_GROUP( ${TARGET_NAME} FILES ${source} ${includes} ${snark_graphics_qt3d_moc} )

ADD_LIBRARY( ${TARGET_NAME} ${source} ${includes} ${snark_graphics_qt3d_moc} )
SET_TARGET_PROPERTIES( ${TARGET_NAME} PROPERTIES ${snark_LIBRARY_PROPERTIES} )
TARGET_LINK_LIBRARIES( ${TARGET_NAME} ${QT_LIBRARIES} ${Qt3D_LIB} ${snark_ALL_EXTERNAL_LIBRARIES} )

INSTALL( FILES ${includes} DESTINATION ${snark_INSTALL_INCLUDE_DIR}/${PROJECT} )
INSTALL(
   TARGETS ${TARGET_NAME}
   RUNTIME DESTINATION ${snark_INSTALL_BIN_DIR} COMPONENT Runtime     # .exe, .dll
   LIBRARY DESTINATION ${snark_INSTALL_LIB_DIR} COMPONENT Runtime     # .so, mod.dll
   ARCHIVE DESTINATION ${snark_INSTALL_LIB_DIR} COMPONENT Development # .a, .lib
)
