# Reference study

Current UI direction is the user's compact Total Commander screenshot / Midnight Commander workflow. The earlier sidebar-tree interpretation was rejected and is not part of the frontend.

Studied on 2026-09-21. This is a targeted source review, not a runtime evaluation of Double Commander.

## Double Commander

Pinned revision: `1f9f9893c2c2f718b66367380a9ea6424cae7475`.

Double Commander is a cross-platform, two-panel file manager inspired by Total Commander. Its Pascal/Lazarus implementation is a reference for behavior and boundaries; aicmd will be an original C++ implementation. The repository identifies GPL-2.0 licensing; no source, icons, or other assets have been imported. [Repository](https://github.com/doublecmd/doublecmd), [license](https://github.com/doublecmd/doublecmd/blob/1f9f9893c2c2f718b66367380a9ea6424cae7475/LICENSE.md).

| Inspected source | Observation | aicmd design consequence |
|---|---|---|
| [ufilesource.pas](https://github.com/doublecmd/doublecmd/blob/1f9f9893c2c2f718b66367380a9ea6424cae7475/src/filesources/ufilesource.pas) | File sources advertise properties and supported operations; this interface also contains GUI handlers and graphics types. | Use a small provider interface with capability discovery. Keep all GUI types outside it. |
| [ufilesourceoperation.pas](https://github.com/doublecmd/doublecmd/blob/1f9f9893c2c2f718b66367380a9ea6424cae7475/src/filesources/ufilesourceoperation.pas) | Operations have synchronized state, worker coordination, and questions for a UI. | Jobs produce structured progress and conflict events; a frontend never needs to be present. |
| [ufileview.pas](https://github.com/doublecmd/doublecmd/blob/1f9f9893c2c2f718b66367380a9ea6424cae7475/src/fileviews/ufileview.pas) | A control owns navigation and file-list lifecycle. | Move navigation, selection, and snapshots into a display-independent session model. |
| [ufileviewworker.pas](https://github.com/doublecmd/doublecmd/blob/1f9f9893c2c2f718b66367380a9ea6424cae7475/src/fileviews/ufileviewworker.pas) | Background list construction supports abort and handoff to the UI thread. | Enumerate in workers, publish immutable results, reject stale generations. |
| [ufileviewnotebook.pas](https://github.com/doublecmd/doublecmd/blob/1f9f9893c2c2f718b66367380a9ea6424cae7475/src/ufileviewnotebook.pas) | Tab pages retain view state, lock modes, and configuration. | Independent persistent tab state; defer lock modes until after basic browsing. |
| [umaincommands.pas](https://github.com/doublecmd/doublecmd/blob/1f9f9893c2c2f718b66367380a9ea6424cae7475/src/umaincommands.pas) | Named actions cover navigation and multi-tab operations. | One typed command registry serves keyboard, menus, terminal, and tools. |
| [ufilesystemlistoperation.pas](https://github.com/doublecmd/doublecmd/blob/1f9f9893c2c2f718b66367380a9ea6424cae7475/src/filesources/filesystem/ufilesystemlistoperation.pas) | Local listing is a concrete file-source operation. | Implement local files first; add remote/archive providers only when needed. |

Keep the two-panel workflow, directory history, tabs, explicit operations, and background work. Defer archive support, plugin compatibility, thumbnail grids, synchronization, and large settings surfaces. Those are not prerequisites for a useful first browser.

## jptxt

Local checkout: `F:/devel/jptxt`; observed HEAD: `69fb0ccb39ca59fabf87390b01b233ad4c045c9e`.

Read `README.md`, `CMakeLists.txt`, `obsidian/Choices.md`, `src/jptxt.h`, and the platform entry-point/argument-handling code.

- C++17 and CMake; Win32/GDI, Cocoa/Objective-C++, and X11 backends.
- Shared document/application code, small platform adapters, static CRT on Windows, and headless self-test/benchmark entry points.
- Project philosophy: speed, then weight, then features. aicmd adopts this as a design preference; jptxt's reported measurements are not aicmd measurements.
- Shared drawing callbacks are useful for compact custom views. aicmd uses native Windows tree/list/tab controls initially for keyboard behavior and accessibility, while retaining platform-independent state.
- jptxt currently accepts filenames and has binary hex viewing, but no explicit `--readonly` CLI mode. Its GUI single-instance handoff must also carry any future viewer/edit mode; passing an unimplemented flag is insufficient.
- Reuse initially means launching the installed editor with a literal file argument. Embedding its document engine or adding terminal rendering would be separate work.

## Model transports

The [Gemini function-calling documentation](https://ai.google.dev/gemini-api/docs/function-calling) describes function declarations and structured arguments. aicmd will generate declarations from its own registry, leaving model names and API details in an adapter.

The [Live tool documentation](https://ai.google.dev/gemini-api/docs/live-api/tools) requires the client to return tool responses. Its support table distinguishes synchronous-only Gemini 3.1 Flash Live from asynchronous-capable models, while its examples include a Gemini 3.8 Live identifier. Do not assume identical capabilities across these model families: configure the model and verify support during adapter integration. Return a job ID promptly, then use job status calls; asynchronous model support is optional.

[MCP tools](https://modelcontextprotocol.io/specification/2025-11-25/server/tools) provide a discovery/call interface with input and output schemas. Use this for the general-purpose external tool server. Gemini Live gets a separate mapping to the same dispatcher; it need not speak MCP itself.
