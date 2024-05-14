idf_component_register(
    SRC_DIRS
        ./
    INCLUDE_DIRS
        "include"
    REQUIRES
        driver
        fatfs
        spiffs
        esp-box-3
        )

component_compile_options(-w)
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")
