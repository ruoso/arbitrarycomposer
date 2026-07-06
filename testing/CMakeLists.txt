# arbc-testing: the contract conformance suite (doc 16:31-44), the crown-jewel
# shipped artifact. A STATIC library, separate from the umbrella `arbc` and
# never linked by it (doc 17:14). Downstream test binaries and plugin authors
# link it alongside `arbc` and call `arbc::contract_tests(factory)`.
#
# DEPENDS lists the full public-header closure it compiles against (doc 17:53):
# the L3 contract plus its transitive lowers. Only their include directories
# are consumed -- their objects resolve at the consumer's final link against
# `arbc` (see arbc_add_testing_library).
arbc_add_testing_library(
  NAME arbc-testing
  SOURCES contract_tests.cpp
  PUBLIC_HEADERS arbc/testing/contract_tests.hpp
  DEPENDS base media surface model contract)
