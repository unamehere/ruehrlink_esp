# PlatformIO-Vorskript: Ist ../VERSION neuer als der letzte CMake-Lauf, wird neu konfiguriert.
# Sonst bleibt die alte Versionsnummer im Build (PlatformIO beachtet CMAKE_CONFIGURE_DEPENDS nicht).
import os

Import("env")  # noqa: F821 – von PlatformIO bereitgestellt

project = env.subst("$PROJECT_DIR")  # noqa: F821
version = os.path.join(project, "..", "VERSION")
cache = os.path.join(env.subst("$BUILD_DIR"), "CMakeCache.txt")  # noqa: F821
if os.path.exists(cache) and os.path.getmtime(version) > os.path.getmtime(cache):
    print("VERSION geaendert - CMake wird neu konfiguriert")
    os.utime(os.path.join(project, "CMakeLists.txt"))
