# Binding generation

ImGuiQuic uses [`dear_bindings`](https://github.com/dearimgui/dear_bindings) to
generate its C API and machine-readable metadata from the pinned Dear ImGui
docking revision.

Regenerate the complete binding surface from the repository root:

```bash
python3 tools/dear_bindings/generate.py
```

The script uses temporary, pinned checkouts and emits:

- `include/imgui_quic_imgui.h`
- `src/imgui_quic_imgui.cpp`
- `tools/dear_bindings/imgui_quic_imgui.json`

Commit the metadata and every changed generated artifact together. Keep
backend-private transport and frame helpers in `src/imgui_quic_internal.h`.
