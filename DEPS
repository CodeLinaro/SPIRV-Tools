use_relative_paths = True

vars = {
  'github': 'https://github.com',

  'abseil_revision': '372124e6af36a540e74a2ec31d79d7297a831f98',

  'effcee_revision': '0f8f491dcbd802c0f361e675bea90c2ea4930c9d',

  'googletest_revision': 'ff233bdd4cac0a0bf6e5cd45bda3406814cb2796',

  # Use protobufs before they gained the dependency on abseil
  'protobuf_revision': 'v21.12',

  're2_revision': '6dcd83d60f7944926bfd308cc13979fc53dd69ca',

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

