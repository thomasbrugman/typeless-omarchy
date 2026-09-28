# Sourced by build.sh: sources/flags for the shared typeless-atspi-helper client
# compiled into libInputHelper.so (helpers/linux-shared).
A11Y_SRCS=("$SHARED/a11y_client.cpp" "$SHARED/native_child.cpp" "$SHARED/a11y_ipc.cpp")
A11Y_CFLAGS=(-I"$SHARED")
# native_child_guard.c is C: compile it separately and link the object.
"$CC" "${COMMON_FLAGS[@]}" -std=gnu11 -fvisibility=hidden -c "$SHARED/native_child_guard.c" \
    -o "$OBJ/native_child_guard.o"
A11Y_SRCS+=("$OBJ/native_child_guard.o")
A11Y_LIBS=()
