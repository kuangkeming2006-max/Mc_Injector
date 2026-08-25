# Third-party notices

This distribution dynamically links Qt 6 libraries from the Qt open-source
distribution. Qt is available under the GNU Lesser General Public License
version 3 (and alternative licenses). Corresponding Qt source releases are
available from <https://download.qt.io/official_releases/qt/>. The deployed Qt
DLLs remain replaceable files next to the application.

The native agent statically incorporates:

- MinHook 1.3.4, copyright Tsuda Kageyu and contributors, BSD 2-Clause license.
- Dear ImGui 1.92.9, copyright Omar Cornut and contributors, MIT license.

Their complete license texts are in the `licenses` directory.

The `runtime` directory is a `jlink` image made from Microsoft Build of
OpenJDK 21.0.10. Its module-specific license and third-party notices are kept
unchanged under `runtime/legal`.
