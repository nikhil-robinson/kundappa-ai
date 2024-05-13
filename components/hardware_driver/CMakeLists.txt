idf_component_register(
    SRC_DIRS
        ./
    INCLUDE_DIRS
        "include"
        "boards/include"
    REQUIRES
        driver
        fatfs
        spiffs
        esp-bsp
        )

component_compile_options(-w)
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")
