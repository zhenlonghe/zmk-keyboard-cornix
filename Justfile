default:
    @just --list --unsorted

config := absolute_path('config')
build := absolute_path('.build')
out := absolute_path('firmware')
draw := absolute_path('draw')
kb := absolute_path('kb')

## you should set ZMK_LIB_PREFIX to your zmk lib path's parent directory
module_base := "${ZMK_LIB_PREFIX:=zmk_exts}"
zmk_base := "${ZMK_LIB_PREFIX}/zmk/app"

# parse combos.dtsi and adjust settings to not run out of slots
_parse_combos:
    #!/usr/bin/env bash
    set -euo pipefail
    cconf="{{ config / 'combos.dtsi' }}"
    if [[ -f $cconf ]]; then
        # set MAX_COMBOS_PER_KEY to the most frequent combos count
        count=$(
            tail -n +10 $cconf |
                grep -Eo '[LR][TMBH][0-9]' |
                sort | uniq -c | sort -nr |
                awk 'NR==1{print $1}'
        )
        sed -Ei "/CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY/s/=.+/=$count/" "{{ config }}"/*.conf
        echo "Setting MAX_COMBOS_PER_KEY to $count"

        # set MAX_KEYS_PER_COMBO to the most frequent key count
        count=$(
            tail -n +10 $cconf |
                grep -o -n '[LR][TMBH][0-9]' |
                cut -d : -f 1 | uniq -c | sort -nr |
                awk 'NR==1{print $1}'
        )
        sed -Ei "/CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO/s/=.+/=$count/" "{{ config }}"/*.conf
        echo "Setting MAX_KEYS_PER_COMBO to $count"
    fi

# parse build.yaml and filter targets by expression
_parse_targets $expr:
    #!/usr/bin/env bash
    attrs="[.board, .shield, .snippet, .\"artifact-name\"]"
    filter="(($attrs | map(. // [.]) | combinations), ((.include // {})[] | $attrs)) | join(\",\")"
    echo "$(yq -r "$filter" build.yaml | grep -v "^," | grep -i "${expr/#all/.*}")"

# build firmware for single board & shield combination
# TODO use artifact
_build_single $board $shield $snippet $artifact *west_args:
    #!/usr/bin/env bash
    set -euo pipefail

    # never set ZEPHYR_BASE, use Zephyr_DIR instead
    # Zephyr_DIR should be xxx/zephyr/share/zephyr-package/cmake/
    if [[ -n "${ZEPHYR_BASE:-}" ]]; then
        echo "ZEPHYR_BASE is set to $ZEPHYR_BASE, but it should not be set when using west."
        exit 1
    fi

    artifact="${artifact:-${shield:+${shield// /+}-}${board}}"

    build_dir="{{ build / '$artifact' }}"

    CMAKE_ARGS=""

    if [[ -f zephyr/module.yml ]] ; then
        module_path_ext="$(pwd)/"
        echo "Found local module $module_path_ext, append to west build command"
        export ZMK_EXTRA_MODULES=$(pwd)
        CMAKE_ARGS="${CMAKE_ARGS} -DZMK_EXTRA_MODULES=$(pwd)"
    fi
    echo "Building firmware for $artifact..."
    echo "Running" west build -s {{zmk_base}} -d "$build_dir" -b $board {{ west_args }} ${snippet:+-S "$snippet"} -- \
        -DZMK_CONFIG="{{ config }}" $CMAKE_ARGS ${shield:+-DSHIELD="$shield"}
    west build -s {{zmk_base}} -d "$build_dir" -b $board {{ west_args }} ${snippet:+-S "$snippet"} -- \
        -DZMK_CONFIG="{{ config }}" ${CMAKE_ARGS} ${shield:+-DSHIELD="$shield"}

    if [[ -f "$build_dir/zephyr/zmk.uf2" ]]; then
        build_output="$build_dir/zephyr/zmk.uf2"
        build_artifact="{{ out }}/$artifact.uf2"
    else
        build_output="$build_dir/zephyr/zmk.bin"
        build_artifact="{{ out }}/$artifact.bin"
    fi
    mkdir -p "{{ out }}" && cp "$build_output" "$build_artifact"
    echo "$build_output saved to $build_artifact"
    cp $build_dir/compile_commands.json ./
    echo "copy clangd db json from $build_dir/compile_commands.json"

# build firmware for matching targets
build expr *west_args: _parse_combos
    #!/usr/bin/env bash
    set -euo pipefail
    targets=$(just _parse_targets {{ expr }})


    [[ -z $targets ]] && echo "No matching targets found. Aborting..." >&2 && exit 1
    echo "$targets" | while IFS="," read -r board shield snippet artifact; do
        just _build_single "$board" "$shield" "$snippet" "$artifact" {{ west_args }}
    done

# clear build cache and artifacts
clean:
    rm -rf {{ build }} {{ out }}

# clear all automatically generated files
clean-all: clean
    rm -rf .west zmk

# clear nix cache
clean-nix:
    nix-collect-garbage --delete-old

# parse & plot keymap
draw keyboard: 
    #!/usr/bin/env bash
    set -euo pipefail
    echo "generated yaml"     
    set -x
    ## should use -z for zmk keyboards
    keymap -c "{{ draw }}/config-{{ keyboard }}.yaml" parse -z "{{ config }}/{{ keyboard }}.keymap" --virtual-layers Combos >"{{ draw }}/{{ keyboard }}.yaml"
    KBOARD=`yq -r '.layout."zmk_keyboard"' {{ draw }}/{{ keyboard }}.yaml`
    echo "found zmk keyboard name : ${KBOARD}"
    #yq -Yi '.combos.[].l = ["Combos"]' "{{ draw }}/{{ keyboard }}.yaml"
    echo "generated svg for ${KBOARD}"     
    keymap -c "{{ draw }}/config-{{ keyboard }}.yaml" draw "{{ draw }}/{{ keyboard }}.yaml" -z "${KBOARD}" >"{{ draw }}/{{ keyboard }}.svg"

# initialize west
init:
    west init -l config
    west update --fetch-opt=--filter=blob:none
    west zephyr-export

# list build targets
list:
    #!/usr/bin/env python3
    import subprocess

    # Run the command and capture its output
    result = subprocess.run(['just', '_parse_targets', 'all'], capture_output=True, text=True, check=True)

    # Split output into a list by lines
    arr = result.stdout.splitlines()

    # Print each line
    for item in arr:
        (board, shield, snippet, artifact) = item.split(',')
        print(f"{artifact}:\n\tboard={board}, shield={shield}, snippet={snippet}")
list_py:
    #!/usr/bin/env python3
    import subprocess

    # Run the command and capture its output
    result = subprocess.run(['just', '_parse_targets', 'all'], capture_output=True, text=True, check=True)

    # Split output into a list by lines
    arr = result.stdout.splitlines()

    # Print each line
    for item in arr:
        (board, shield, snippet, artifact) = item.split(',')
        print(f"{artifact}:\n\tboard={board}, shield={shield}, snippet={snippet}")


# update west
update: update-config
    west config zephyr.base -- "{{module_base}}/zephyr" 
    west update --fetch-opt=--filter=blob:none

update-config:
    #west config build.cmake-args -- "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DZMK_EXTRA_MODULES=$(pwd)" 
    west config build.cmake-args -- "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON" 
# upgrade zephyr-sdk and python dependencies
upgrade-sdk:
    nix flake update --flake .

[no-cd]
test $testpath *FLAGS:
    #!/usr/bin/env bash
    set -euo pipefail
    testcase=$(basename "$testpath")
    build_dir="{{ build / "tests" / '$testcase' }}"
    config_dir="{{ '$(pwd)' / '$testpath' }}"
    cd {{ justfile_directory() }}

    if [[ "{{ FLAGS }}" != *"--no-build"* ]]; then
        echo "Running $testcase..."
        rm -rf "$build_dir"
        west build -s {{zmk_base}} -d "$build_dir" -b native_posix_64 -- \
            -DCONFIG_ASSERT=y -DZMK_CONFIG="$config_dir"
    fi

    ${build_dir}/zephyr/zmk.exe | sed -e "s/.*> //" |
        tee ${build_dir}/keycode_events.full.log |
        sed -n -f ${config_dir}/events.patterns > ${build_dir}/keycode_events.log
    diff -auZ ${config_dir}/keycode_events.snapshot ${build_dir}/keycode_events.log

    if [[ "{{ FLAGS }}" == *"--verbose"* ]]; then
        cat ${build_dir}/keycode_events.log
    fi

    if [[ "{{ FLAGS }}" == *"--auto-accept"* ]]; then
        cp ${build_dir}/keycode_events.log ${config_dir}/keycode_events.snapshot
    fi
