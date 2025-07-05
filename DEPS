use_relative_paths = True

vars = {
  'github': 'https://github.com',

  'abseil_revision': 'daff1b1a9cd5cf32c7d5409b4844e32e92dde461',

  'effcee_revision': '8ce15c424e61a94ee27b5be0ec0ed036b158e6e3',

  'googletest_revision': '3983f67e32fb3e9294487b9d4f9586efa6e5d088',

  # Use protobufs before they gained the dependency on abseil
  'protobuf_revision': 'v21.12',

  're2_revision': '8ea5841693c6c0b837c6ed2189217e8f8d6fee9c',

  'spirv_headers_revision': 'fd96661925488574fe247a779babe5d380b63635',
}

deps = {
  'external/abseil_cpp':
      Var('github') + '/abseil/abseil-cpp.git@' + Var('abseil_revision'),

  'external/effcee':
      Var('github') + '/google/effcee.git@' + Var('effcee_revision'),

  'external/googletest':
      Var('github') + '/google/googletest.git@' + Var('googletest_revision'),

  'external/protobuf':
      Var('github') + '/protocolbuffers/protobuf.git@' + Var('protobuf_revision'),

  'external/re2':
      Var('github') + '/google/re2.git@' + Var('re2_revision'),

  'external/spirv-headers':
      Var('github') +  '/KhronosGroup/SPIRV-Headers.git@' +
          Var('spirv_headers_revision'),
}

