# Development Notes

## Build Instructions

Windows build and setup instructions are available in the OpenXRay wiki:

https://github.com/OpenXRay/xray-16/wiki/%5BEN%5D-How-to-build-and-setup-on-Windows

## Debug/Run From Visual Studio

1. Select the engine solution `src/engine.sln` and open its properties.
2. Select `Startup Project`.
3. Select `Single startup project`.
4. Select `xr_3da`.
5. Select the `xr_3da` project and open its properties.
6. Select `Debugging`.
7. Set `Working Directory` to the game folder that contains `fsgame.ltx`, for example:

```text
C:\PATH\TO\StalkerCallOfPripyat\
```

## Start Directly From a Save

One practical debug flow is:

1. Start a new game normally.
2. Create a save.
3. In Visual Studio, open `xr_3da` project properties.
4. Go to `Debugging`.
5. Set `Command Arguments` to:

```text
-nosplash -nointro -start server(your_save_name/single/alife/load) client(localhost)
```

Replace `your_save_name` with the save name without the `.sav` extension.

The `your_save_name/single/alife/load` part is important. It is not a "choose one of these options" list. It is a positional `start server(...)` format:

```text
<game_or_spawn>/<game_type>/<alife>/<new_or_load>
```

For loading a save:

```text
your_save_name/single/alife/load
```

means:

- `your_save_name` is the save name.
- `single` selects singleplayer game type.
- `alife` starts the game with ALife.
- `load` tells the engine to load the save instead of starting a new game.

Keep the syntax exact:

- Use `server(...)`, not `server (...)`.
- Use `client(...)`, not `client (...)`.
- Use lowercase `server`, `client`, `single`, `alife`, and `load`.
- Pass the save name without `.sav`.

Example:

```text
-nosplash -nointro -start server(opensave/single/alife/load) client(localhost)
```
