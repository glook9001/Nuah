if(NOT DEFINED JAVAP OR NOT DEFINED ATL_JAR)
  message(FATAL_ERROR "JAVAP and ATL_JAR are required")
endif()

execute_process(
  COMMAND "${JAVAP}" -classpath "${ATL_JAR}" android.view.InputDevice
  RESULT_VARIABLE javap_status
  OUTPUT_VARIABLE input_device_api
  ERROR_VARIABLE javap_error)
if(NOT javap_status EQUAL 0)
  message(FATAL_ERROR "cannot inspect ATL InputDevice: ${javap_error}")
endif()

foreach(required_method
    "getMotionRanges()"
    "getMotionRange(int)"
    "getMotionRange(int, int)"
    "supportsSource(int)")
  string(FIND "${input_device_api}" "${required_method}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "ATL InputDevice is missing ${required_method}")
  endif()
endforeach()

execute_process(
  COMMAND "${JAVAP}" -classpath "${ATL_JAR}" android.view.InputDevice$MotionRange
  RESULT_VARIABLE range_status
  OUTPUT_VARIABLE motion_range_api
  ERROR_VARIABLE range_error)
if(NOT range_status EQUAL 0)
  message(FATAL_ERROR "cannot inspect ATL MotionRange: ${range_error}")
endif()

foreach(required_method "getAxis()" "getSource()" "getMin()" "getMax()")
  string(FIND "${motion_range_api}" "${required_method}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "ATL MotionRange is missing ${required_method}")
  endif()
endforeach()
