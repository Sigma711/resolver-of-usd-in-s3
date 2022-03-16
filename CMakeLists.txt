project(s3-usd-resolver)

cmake_minimum_required(VERSION 3.1)

set(CMAKE_CXX_STANDARD 17)
add_compile_options(-O3 -Wall -DBUILD_OPTLEVEL_OPT -DBUILD_COMPONENT_SRC_PREFIX="")

include_directories(/mnt/d/Code/JBCode/Cpp/USD)

