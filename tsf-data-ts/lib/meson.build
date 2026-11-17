# Copyright (C) 2025-2026 Interpretica Unipessoal Lda

# The agent-side Python (fixture generator + oracle, the ETL pipeline
# under test) is embedded into the helpers as C strings, so a test can
# put it onto any agent, local or remote.
py3 = find_program('python3')
py_embed = custom_target('tsapi_py',
    input: files('py/datagen.py', 'py/etl.py'),
    output: 'tsapi_py.h',
    command: [ py3, files('py/embed.py'), '@OUTPUT@', '@INPUT@' ])

sources = [
    'tsapi_data.c',
    py_embed,
]

ts_lib = static_library('ts_lib', sources,
                        dependencies: test_deps,
                        include_directories: lib_dir)
