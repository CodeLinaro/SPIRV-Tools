use_relative_paths = True

vars = {
  'github': 'https://github.com',

  'abseil_revision': 'cb319b3e67ef0802420c5095b7b67026ce392853',

  'effcee_revision': '0f8f491dcbd802c0f361e675bea90c2ea4930c9d',

  'googletest_revision': 'a7f443b80b105f940225332ed3c31f2790092f47',

  # Use protobufs before they gained the dependency on abseil
  'protobuf_revision': 'v21.12',

  're2_revision': '33eba105f662c99c8f7d6a264c84ed350bd1dc39',

  'spirv_headers_revision': 'eb49bb7b1136298b77945c52b4bbbc433f7885de',
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

