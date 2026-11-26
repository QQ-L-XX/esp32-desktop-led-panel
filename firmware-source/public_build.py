"""Keep local build directories out of compiler file-name strings and debug info."""
from pathlib import Path

Import("env")

prefixes = {
    Path.home().as_posix(): "/home/builder",
    Path(env.subst("$PROJECT_DIR")).as_posix(): "/src/desktop-led-panel",
}
flags = []
for original, replacement in prefixes.items():
    flags.extend([
        "-ffile-prefix-map=" + original + "=" + replacement,
        "-fmacro-prefix-map=" + original + "=" + replacement,
    ])
env.Append(CCFLAGS=flags)
