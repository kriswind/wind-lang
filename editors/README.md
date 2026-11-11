# Editor support

The language server and linter are part of the `wind` binary, so every editor
needs only two things: `wind` on `PATH`, and a client that starts `wind lsp`
for `.wind` files.

```sh
make install          # installs to ~/.local/bin/wind (override with PREFIX=)
```

The server speaks LSP over stdio and provides diagnostics (errors and lint
warnings), hover, go-to-definition and completion.

## Neovim (0.11+)

`editors/nvim` is a runtime directory containing filetype detection, syntax
highlighting and the LSP configuration. Add it to the runtime path; the server
is enabled automatically.

With lazy.nvim:

```lua
{ dir = "/path/to/wind/editors/nvim" }
```

Without a plugin manager:

```lua
vim.opt.runtimepath:prepend("/path/to/wind/editors/nvim")
```

Neovim marks diagnostics in the sign column only by default. To also show the
message at the end of the line:

```lua
vim.diagnostic.config({ virtual_text = true })
```

On Neovim older than 0.11, start the client yourself:

```lua
vim.api.nvim_create_autocmd("FileType", {
  pattern = "wind",
  callback = function()
    vim.lsp.start({ name = "wind", cmd = { "wind", "lsp" } })
  end,
})
```

### Mason

Mason only installs binaries; the configuration above is what attaches the
server, and it works unchanged alongside Mason. A `wind` installed with
`make install` is found as long as `~/.local/bin` is on `PATH`.

To have Mason itself build and install `wind`, use the registry in
`editors/mason-registry`. Its package definition builds a tagged release of
this repository, so the version named in
`editors/mason-registry/packages/wind/package.yaml` must exist as a git tag.

1. Clone this repository.
2. Install `yq`, which Mason needs to read a local registry
   (`:MasonInstall yq`).
3. List the registry ahead of the default one:

   ```lua
   require("mason").setup({
     registries = {
       "file:/path/to/wind/editors/mason-registry",
       "github:mason-org/mason-registry",
     },
   })
   ```

4. `:MasonInstall wind`

## Sublime Text

1. Link the syntax package into Sublime's `Packages` directory:

   ```sh
   ln -s /path/to/wind/editors/sublime ~/.config/sublime-text/Packages/Wind
   ```

2. Install the `LSP` package from Package Control, then add the client under
   `Preferences > Package Settings > LSP > Settings`:

   ```json
   {
     "clients": {
       "wind": {
         "enabled": true,
         "command": ["wind", "lsp"],
         "selector": "source.wind"
       }
     }
   }
   ```

   To show messages at the right of the line, and not only as an underline
   and gutter mark, add `"show_diagnostics_annotations_severity_level": 2`
   to the same settings file (1 shows errors only, 2 adds warnings).

   Sublime started from a desktop launcher does not read the shell's `PATH`.
   If it reports that `wind` cannot be found, use the absolute path in
   `command`, for example `/home/you/.local/bin/wind`.

## Helix

In `~/.config/helix/languages.toml`:

```toml
[language-server.wind]
command = "wind"
args = ["lsp"]

[[language]]
name = "wind"
scope = "source.wind"
file-types = ["wind"]
comment-token = "//"
language-servers = ["wind"]
```

## Anything else

Any LSP client works: the command is `wind lsp`, the transport is stdio, and
the language id is `wind`.

Tools that parse compiler output instead (SublimeLinter, nvim-lint, efm,
`:make`) can run `wind check <file>`, which prints one diagnostic per line as
`file:line:col: severity: message` and exits with status 1 if there are
errors.
