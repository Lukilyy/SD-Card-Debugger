# Preserve the build helpers exported by the ESP-IDF FatFs component.
file(TO_CMAKE_PATH "$ENV{IDF_PATH}" IDF_PATH_CMAKE)
include("${IDF_PATH_CMAKE}/components/fatfs/project_include.cmake")
