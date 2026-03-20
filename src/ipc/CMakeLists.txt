# IPC 라이브러리 (TASK_36: Shared Memory SPSC Queue)
add_library(ipc
    shm_manager.cpp
)

target_link_libraries(ipc
    PUBLIC
        common
        rt
        Threads::Threads
)
