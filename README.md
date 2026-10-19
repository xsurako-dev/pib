# Predictive Input Buffer

Frame-adjustable, input-buffered hazard assist for the Cube and the Wave (Geometry Dash, Geode mod).

## Getting the .geode file (no local setup)

1. Create a new GitHub repository and upload everything in this folder, keeping the `.github` folder.
2. Open the repo's **Actions** tab and run **Build Geode Mod** (it also runs automatically on every push).
3. When the run finishes, download **Build Output**. It contains the `.geode` file.

## Building locally instead

1. Install the Geode CLI and SDK (https://docs.geode-sdk.org/getting-started/).
2. Set the `GEODE_SDK` environment variable.
3. In this folder run `geode build`. The `.geode` file appears in `build/`.

## Installing

1. Install Geode from the official site, https://geode-sdk.org.
2. Copy the `.geode` file into the `geode/mods` folder inside your Geometry Dash install.
3. Launch Geometry Dash normally. The mod loads automatically and its settings are under Geode > Mods > Predictive Input Buffer.

## Before building

In `mod.json`, change `id` and `developer`, and make sure `geode` and the `gd` versions match the Geode and Geometry Dash versions you are using. If the build or the mod fails to load with a version error, this is the first thing to check.
