use_relative_paths = True

vars = {
  'github': 'https://github.com',

  'abseil_revision': '2f61aed18c4affd3a75a2b2d2468d23f2f16192a',

  'effcee_revision': '19b4aa87af25cb4ee779a071409732f34bfc305c',

  'googletest_revision': 'a7f443b80b105f940225332ed3c31f2790092f47',

  # Use protobufs before they gained the dependency on abseil
  'protobuf_revision': 'v21.12',

  're2_revision': 'e29afa1844861971a4cbf5fe559069b42161e97a',

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

