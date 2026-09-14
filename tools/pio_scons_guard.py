"""
PlatformIO pre-build script: keep Core's SCons package alive.

Workaround for https://github.com/pioarduino/platform-espressif32/issues/529.
PlatformIO Core >= 6.2.0 ships SCons 4.11.1 as the core package `tool-scons`,
while the pioarduino platform (all releases up to 55.03.311) expects SCons 4.8.1
and deletes ~/.platformio/packages/tool-scons during platform configuration
when the versions differ. The build then dies at the link step with
"No module named 'SCons.Tool.FortranCommon'" because the running SCons can no
longer lazily import its own modules.

Platform configuration happens before `pre:` scripts, so the deletion cannot be
prevented from a project - but the package can be put back before anything
needs it. Core reinstalls it from its download cache at the same path.
Remove this script (and the extra_scripts line) once pioarduino is fixed.
"""
Import("env")  # noqa: F821  (SCons construction environment)

import os  # noqa: E402


def _restore_tool_scons():
    try:
        from platformio.package.manager.core import get_core_package_dir
    except Exception as exc:  # very old core: nothing to do
        print("scons guard: cannot import core package manager (%s)" % exc)
        return
    try:
        path = get_core_package_dir("tool-scons")  # installs if missing
    except Exception as exc:
        print("scons guard: failed to restore tool-scons: %s" % exc)
        return
    if not path or not os.path.isdir(path):
        print("scons guard: tool-scons still missing - build will likely fail at link")
        return
    # The running SCons must find its own modules again: compare locations.
    try:
        import SCons  # noqa: E402
        live = os.path.dirname(os.path.dirname(os.path.abspath(SCons.__file__)))
        if not os.path.realpath(live).startswith(os.path.realpath(path)):
            print("scons guard: WARNING restored tool-scons (%s) is not where the running SCons lives (%s)"
                  % (path, live))
        elif not os.path.isfile(os.path.join(live, "SCons", "Tool", "FortranCommon.py")):
            print("scons guard: WARNING restored package lacks SCons/Tool/FortranCommon.py")
    except Exception as exc:
        print("scons guard: could not verify SCons location (%s)" % exc)


_restore_tool_scons()
