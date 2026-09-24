#include <stdbool.h>
#include <stdint.h>

#include <kos/crypto.h>
#include <kos/gui_repository.h>
#include <kos/memory.h>

static bool repository_id_is_valid(const char *repository_id) {
    if (repository_id == NULL || repository_id[0] == '\0') return false;
    for (uint64_t index = 0; repository_id[index] != '\0'; ++index) {
        char character = repository_id[index];
        if (!((character >= 'a' && character <= 'z')
              || (character >= 'A' && character <= 'Z')
              || (character >= '0' && character <= '9')
              || character == '-' || character == '_' || character == '.')) {
            return false;
        }
    }
    return true;
}

static const struct package_verifier_ops sha256_only_ops = {
    .verify_checksum = package_verifier_sha256_hex,
};

static enum gui_repository_status self_test_repository_lookup(
    void *context, const char *gui_id, struct gui_manifest *manifest_out) {
    if (context == NULL || gui_id == NULL || manifest_out == NULL) {
        return GUI_REPOSITORY_ERR_INVALID_ARGUMENT;
    }
    const struct gui_manifest *fixture = context;
    if (kstrcmp(fixture->id, gui_id) != 0) return GUI_REPOSITORY_ERR_NOT_FOUND;
    *manifest_out = *fixture;
    return GUI_REPOSITORY_OK;
}

static const struct gui_repository_ops self_test_repository_ops = {
    .lookup = self_test_repository_lookup,
};

static uint8_t checksum_hex_value(char character) {
    if (character >= '0' && character <= '9') return (uint8_t)(character - '0');
    if (character >= 'a' && character <= 'f') return (uint8_t)(character - 'a' + 10);
    if (character >= 'A' && character <= 'F') return (uint8_t)(character - 'A' + 10);
    return UINT8_MAX;
}

enum gui_repository_status gui_repository_lookup(
    const struct gui_repository *repository, const char *gui_id,
    struct gui_manifest *manifest_out) {
    if (repository == NULL || gui_id == NULL || gui_id[0] == '\0'
        || manifest_out == NULL
        || !repository_id_is_valid(repository->repository_id)) {
        return GUI_REPOSITORY_ERR_INVALID_ARGUMENT;
    }
    *manifest_out = (struct gui_manifest){0};
    if (repository->ops == NULL || repository->ops->lookup == NULL) {
        return GUI_REPOSITORY_ERR_NOT_CONFIGURED;
    }

    enum gui_repository_status status = repository->ops->lookup(
        repository->context, gui_id, manifest_out);
    if (status != GUI_REPOSITORY_OK) {
        *manifest_out = (struct gui_manifest){0};
        return status;
    }
    if (!gui_manifest_is_valid(manifest_out)
        || manifest_out->source != GUI_PACKAGE_SOURCE_REMOTE
        || kstrcmp(manifest_out->id, gui_id) != 0
        || kstrcmp(manifest_out->repository_id, repository->repository_id) != 0) {
        *manifest_out = (struct gui_manifest){0};
        return GUI_REPOSITORY_ERR_INVALID_MANIFEST;
    }
    return GUI_REPOSITORY_OK;
}

enum gui_repository_status remote_package_provider_fetch(
    const struct remote_package_provider *provider,
    const struct gui_manifest *manifest,
    struct remote_package_artifact *artifact_out) {
    if (provider == NULL || manifest == NULL || artifact_out == NULL
        || !gui_manifest_is_valid(manifest)
        || manifest->source != GUI_PACKAGE_SOURCE_REMOTE) {
        return GUI_REPOSITORY_ERR_INVALID_ARGUMENT;
    }
    *artifact_out = (struct remote_package_artifact){0};
    if (provider->ops == NULL || provider->ops->fetch == NULL
        || provider->ops->release == NULL) {
        return GUI_REPOSITORY_ERR_NOT_CONFIGURED;
    }

    enum gui_repository_status status = provider->ops->fetch(
        provider->context, manifest, artifact_out);
    if (status != GUI_REPOSITORY_OK) {
        remote_package_provider_release(provider, artifact_out);
        return status;
    }
    if (artifact_out->data == NULL || artifact_out->size == 0) {
        remote_package_provider_release(provider, artifact_out);
        return GUI_REPOSITORY_ERR_INVALID_ARTIFACT;
    }
    return GUI_REPOSITORY_OK;
}

void remote_package_provider_release(
    const struct remote_package_provider *provider,
    struct remote_package_artifact *artifact) {
    if (artifact == NULL) return;
    if (provider != NULL && provider->ops != NULL
        && provider->ops->release != NULL
        && (artifact->data != NULL || artifact->provider_token != NULL)) {
        provider->ops->release(provider->context, artifact);
    }
    *artifact = (struct remote_package_artifact){0};
}

void package_verifier_init_sha256_only(struct package_verifier *verifier) {
    if (verifier == NULL) return;
    *verifier = (struct package_verifier){
        .ops = &sha256_only_ops,
        .context = NULL,
    };
}

enum package_verification_status package_verifier_sha256_hex(
    void *context, const char *expected_sha256,
    const uint8_t *package_data, uint64_t package_size) {
    (void)context;
    if (expected_sha256 == NULL || package_data == NULL || package_size == 0) {
        return PACKAGE_VERIFICATION_ERR_INVALID_ARGUMENT;
    }
    for (uint32_t index = 0; index < 64; ++index) {
        if (checksum_hex_value(expected_sha256[index]) == UINT8_MAX) {
            return PACKAGE_VERIFICATION_ERR_INVALID_ARGUMENT;
        }
    }
    if (expected_sha256[64] != '\0') {
        return PACKAGE_VERIFICATION_ERR_INVALID_ARGUMENT;
    }

    uint8_t digest[32];
    kos_sha256(package_data, package_size, digest);
    for (uint32_t index = 0; index < 32; ++index) {
        uint8_t high = checksum_hex_value(expected_sha256[index * 2]);
        uint8_t low = checksum_hex_value(expected_sha256[index * 2 + 1]);
        if (high != (digest[index] >> 4) || low != (digest[index] & 0x0Fu)) {
            return PACKAGE_VERIFICATION_ERR_CHECKSUM;
        }
    }
    return PACKAGE_VERIFICATION_OK;
}

enum package_verification_status package_verifier_verify_checksum(
    const struct package_verifier *verifier, const char *expected_sha256,
    const uint8_t *package_data, uint64_t package_size) {
    if (verifier == NULL || expected_sha256 == NULL || package_data == NULL
        || package_size == 0) {
        return PACKAGE_VERIFICATION_ERR_INVALID_ARGUMENT;
    }
    if (verifier->ops == NULL || verifier->ops->verify_checksum == NULL) {
        return PACKAGE_VERIFICATION_ERR_NOT_CONFIGURED;
    }
    return verifier->ops->verify_checksum(verifier->context, expected_sha256,
                                           package_data, package_size);
}

enum package_verification_status package_verifier_verify_signature(
    const struct package_verifier *verifier,
    const struct gui_manifest *manifest,
    const uint8_t *package_data, uint64_t package_size) {
    if (verifier == NULL || manifest == NULL || package_data == NULL
        || package_size == 0) {
        return PACKAGE_VERIFICATION_ERR_INVALID_ARGUMENT;
    }
    if (manifest->signature[0] == '\0') {
        return PACKAGE_VERIFICATION_ERR_SIGNATURE_MISSING;
    }
    if (verifier->ops == NULL || verifier->ops->verify_signature == NULL) {
        return PACKAGE_VERIFICATION_ERR_NOT_CONFIGURED;
    }
    return verifier->ops->verify_signature(verifier->context, manifest,
                                            package_data, package_size);
}

bool gui_remote_interfaces_self_test(void) {
    static const uint8_t data[] = {'a', 'b', 'c'};
    static const char expected_sha256[] =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    static const char wrong_sha256[] =
        "aa7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    static const char malformed_sha256[] = "abc";

    struct gui_repository repository = {.repository_id = "self-test"};
    struct gui_manifest manifest;
    if (gui_repository_lookup(&repository, "example", &manifest)
            != GUI_REPOSITORY_ERR_NOT_CONFIGURED
        || manifest.id[0] != '\0') return false;

    struct gui_manifest remote_manifest = {
        .id = "example",
        .name = "Test fixture",
        .version = "1.0.0",
        .source = GUI_PACKAGE_SOURCE_REMOTE,
        .requires_network = true,
        .package_app_id = "org.test.example",
        .install_root = "system/gui/example",
        .entry_point = "bin/example.elf",
        .architecture = "x86_64",
        .repository_id = "self-test",
        .checksum_sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        .signature = "self-test-only",
    };

    /* This is an in-memory metadata fixture; it performs no network I/O. */
    repository.ops = &self_test_repository_ops;
    repository.context = &remote_manifest;
    if (gui_repository_lookup(&repository, "example", &manifest)
            != GUI_REPOSITORY_OK
        || kstrcmp(manifest.id, "example") != 0
        || kstrcmp(manifest.repository_id, "self-test") != 0) return false;
    manifest = (struct gui_manifest){.id = "stale-output"};
    if (gui_repository_lookup(&repository, "missing", &manifest)
            != GUI_REPOSITORY_ERR_NOT_FOUND
        || manifest.id[0] != '\0') return false;

    struct gui_manifest saved_manifest = remote_manifest;
    kstrncpy(remote_manifest.repository_id, "other-repository",
             sizeof(remote_manifest.repository_id) - 1);
    manifest = (struct gui_manifest){.id = "stale-output"};
    if (gui_repository_lookup(&repository, "example", &manifest)
            != GUI_REPOSITORY_ERR_INVALID_MANIFEST
        || manifest.id[0] != '\0') return false;
    remote_manifest = saved_manifest;
    repository.repository_id = "bad/repository";
    if (gui_repository_lookup(&repository, "example", &manifest)
        != GUI_REPOSITORY_ERR_INVALID_ARGUMENT) return false;

    struct remote_package_provider provider = {0};
    struct remote_package_artifact artifact = {
        .data = data,
        .size = sizeof(data),
        .provider_token = &remote_manifest,
    };
    if (remote_package_provider_fetch(&provider, &remote_manifest, &artifact)
        != GUI_REPOSITORY_ERR_NOT_CONFIGURED || artifact.data != NULL
        || artifact.size != 0 || artifact.provider_token != NULL) return false;

    struct package_verifier verifier;
    package_verifier_init_sha256_only(&verifier);
    if (package_verifier_verify_checksum(&verifier, expected_sha256, data,
                                         sizeof(data)) != PACKAGE_VERIFICATION_OK
        || package_verifier_verify_checksum(&verifier, wrong_sha256, data,
                                            sizeof(data))
            != PACKAGE_VERIFICATION_ERR_CHECKSUM
        || package_verifier_verify_checksum(&verifier, malformed_sha256, data,
                                            sizeof(data))
            != PACKAGE_VERIFICATION_ERR_INVALID_ARGUMENT
        || package_verifier_verify_signature(&verifier, &remote_manifest, data,
                                             sizeof(data))
            != PACKAGE_VERIFICATION_ERR_NOT_CONFIGURED) return false;
    struct gui_manifest unsigned_manifest = remote_manifest;
    unsigned_manifest.signature[0] = '\0';
    if (package_verifier_verify_signature(&verifier, &unsigned_manifest, data,
                                          sizeof(data))
        != PACKAGE_VERIFICATION_ERR_SIGNATURE_MISSING) return false;
    return true;
}
