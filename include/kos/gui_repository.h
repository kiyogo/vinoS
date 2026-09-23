#ifndef KOS_GUI_REPOSITORY_H
#define KOS_GUI_REPOSITORY_H

#include <stdbool.h>
#include <stdint.h>

#include <kos/installer_contracts.h>

enum gui_repository_status {
    GUI_REPOSITORY_OK = 0,
    GUI_REPOSITORY_ERR_INVALID_ARGUMENT = -1,
    GUI_REPOSITORY_ERR_NOT_CONFIGURED = -2,
    GUI_REPOSITORY_ERR_NOT_FOUND = -3,
    GUI_REPOSITORY_ERR_TRANSPORT = -4,
    GUI_REPOSITORY_ERR_INVALID_MANIFEST = -5,
    GUI_REPOSITORY_ERR_INVALID_ARTIFACT = -6,
};

struct gui_repository_ops {
    enum gui_repository_status (*lookup)(void *context, const char *gui_id,
                                         struct gui_manifest *manifest_out);
};

/* A repository is named by a stable ID. It contains no URL or transport policy. */
struct gui_repository {
    const char *repository_id;
    const struct gui_repository_ops *ops;
    void *context;
};

struct remote_package_artifact {
    const uint8_t *data;
    uint64_t size;
    void *provider_token;
};

struct remote_package_provider_ops {
    enum gui_repository_status (*fetch)(void *context,
                                        const struct gui_manifest *manifest,
                                        struct remote_package_artifact *artifact_out);
    void (*release)(void *context, struct remote_package_artifact *artifact);
};

/* No progress callback is defined until a real provider can report measured I/O. */
struct remote_package_provider {
    const struct remote_package_provider_ops *ops;
    void *context;
};

enum package_verification_status {
    PACKAGE_VERIFICATION_OK = 0,
    PACKAGE_VERIFICATION_ERR_INVALID_ARGUMENT = -1,
    PACKAGE_VERIFICATION_ERR_CHECKSUM = -2,
    PACKAGE_VERIFICATION_ERR_SIGNATURE = -3,
    PACKAGE_VERIFICATION_ERR_NOT_CONFIGURED = -4,
    PACKAGE_VERIFICATION_ERR_SIGNATURE_MISSING = -5,
};

struct package_verifier_ops {
    enum package_verification_status (*verify_checksum)(
        void *context, const char *expected_sha256,
        const uint8_t *package_data, uint64_t package_size);
    enum package_verification_status (*verify_signature)(
        void *context, const struct gui_manifest *manifest,
        const uint8_t *package_data, uint64_t package_size);
};

struct package_verifier {
    const struct package_verifier_ops *ops;
    void *context;
};

enum gui_repository_status gui_repository_lookup(
    const struct gui_repository *repository, const char *gui_id,
    struct gui_manifest *manifest_out);
enum gui_repository_status remote_package_provider_fetch(
    const struct remote_package_provider *provider,
    const struct gui_manifest *manifest,
    struct remote_package_artifact *artifact_out);
void remote_package_provider_release(
    const struct remote_package_provider *provider,
    struct remote_package_artifact *artifact);

void package_verifier_init_sha256_only(struct package_verifier *verifier);
enum package_verification_status package_verifier_sha256_hex(
    void *context, const char *expected_sha256,
    const uint8_t *package_data, uint64_t package_size);
enum package_verification_status package_verifier_verify_checksum(
    const struct package_verifier *verifier, const char *expected_sha256,
    const uint8_t *package_data, uint64_t package_size);
enum package_verification_status package_verifier_verify_signature(
    const struct package_verifier *verifier,
    const struct gui_manifest *manifest,
    const uint8_t *package_data, uint64_t package_size);

bool gui_remote_interfaces_self_test(void);

#endif /* KOS_GUI_REPOSITORY_H */
