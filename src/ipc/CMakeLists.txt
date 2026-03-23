# IPC 라이브러리 (TASK_36: SHM, TASK_42: Unix Socket)
add_library(ipc
    shm_manager.cpp
    unix_socket.cpp
)

target_link_libraries(ipc
    PUBLIC
        common
        rt
        Threads::Threads
)
