/* Host tests for the block layer contract (plan phase I0).
 *
 * The layer is three things: validate, dispatch, and check what came back
 * against what was asked for. Only the third is new, and it is the one these
 * tests are weighted towards — a driver that moves four sectors of eight and
 * returns success is reporting a success that lost half the data, and until
 * now every caller above would have believed it.
 *
 * The other property worth as much is that **every path sets a reason**. A
 * request that comes back untouched must say NOT_ISSUED rather than looking
 * like a success, because the value this replaces was a bare int where a
 * timeout, an absent disk and a request never sent were the same number.
 */

#include <stdio.h>
#include <string.h>

#include "vibeos/blkdev.h"
#include "vibeos/io_stats.h"

int test_blkdev(void);

#define BD_SECTORS  64u
#define BD_SECBYTES 512u

static unsigned char g_disk[BD_SECTORS][BD_SECBYTES];

/* What the fake driver should do next. Set per test, so a driver's misbehaviour
 * is chosen rather than simulated by chance. */
static int      g_fake_rc;
static int      g_fake_short_by;
static vibeos_blk_result_t g_fake_reason;
static uint32_t g_fake_calls;

static int g_fail;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("  blkdev: FAIL %s\n", (what));                            \
            g_fail++;                                                         \
        }                                                                     \
    } while (0)

static int fake_submit(void *ctx, vibeos_blk_request_t *req) {
    uint32_t move;
    uint32_t i;

    (void)ctx;
    g_fake_calls++;

    move = req->sectors;
    if ((uint32_t)g_fake_short_by < move) {
        move -= (uint32_t)g_fake_short_by;
    } else {
        move = 0;
    }
    for (i = 0; i < move; i++) {
        unsigned char *d = g_disk[req->lba + i];
        unsigned char *p = (unsigned char *)req->buf + i * BD_SECBYTES;
        if (req->write) {
            memcpy(d, p, BD_SECBYTES);
        } else {
            memcpy(p, d, BD_SECBYTES);
        }
    }
    req->sectors_done = move;
    if (g_fake_reason != VIBEOS_BLK_NOT_ISSUED) {
        req->result = g_fake_reason;
    }
    return g_fake_rc;
}

static vibeos_blk_driver_t fake_driver(const char *name) {
    vibeos_blk_driver_t d;

    memset(&d, 0, sizeof(d));
    d.name = name;
    d.sector_bytes = BD_SECBYTES;
    d.sectors = BD_SECTORS;
    d.submit = fake_submit;
    d.ctx = 0;
    return d;
}

static uint32_t bd_reset(void) {
    vibeos_blk_driver_t d = fake_driver("fake");
    uint32_t dev = 0xFFFFFFFFu;

    memset(g_disk, 0, sizeof(g_disk));
    vibeos_blk_reset();
    vibeos_io_stats_reset();
    g_fake_rc = 0;
    g_fake_short_by = 0;
    g_fake_reason = VIBEOS_BLK_NOT_ISSUED;
    g_fake_calls = 0;
    if (vibeos_blk_register(&d, &dev) != 0) {
        printf("  blkdev: FAIL register\n");
        g_fail++;
    }
    return dev;
}

/* --- it works ------------------------------------------------------------- */

static void test_round_trip(void) {
    uint32_t dev = bd_reset();
    unsigned char out[BD_SECBYTES * 2u];
    unsigned char in[BD_SECBYTES * 2u];

    memset(out, 0x71, sizeof(out));
    CHECK(vibeos_blk_write(dev, 4, 2, out) == 0, "two sectors written");
    memset(in, 0, sizeof(in));
    CHECK(vibeos_blk_read(dev, 4, 2, in) == 0, "and read back");
    CHECK(memcmp(in, out, sizeof(out)) == 0, "byte for byte");

    CHECK(vibeos_io_stats()->reads == 1u, "one read counted");
    CHECK(vibeos_io_stats()->writes == 1u, "one write counted");
    CHECK(vibeos_io_stats()->sectors_read == 2u, "two sectors read");
    CHECK(vibeos_io_stats()->sectors_written == 2u, "two written");
    CHECK(vibeos_io_stats()->results[VIBEOS_BLK_OK] == 2u, "both ok");
}

/* Two devices are two devices. There is one global today, which is why a
 * machine with a disk and a CD cannot be described. */
static void test_two_devices(void) {
    vibeos_blk_driver_t b = fake_driver("second");
    uint32_t a_dev, b_dev = 0xFFFFFFFFu;

    a_dev = bd_reset();
    CHECK(vibeos_blk_register(&b, &b_dev) == 0, "a second device");
    CHECK(b_dev != a_dev, "with its own index");
    CHECK(vibeos_blk_count() == 2u, "two registered");
    CHECK(strcmp(vibeos_blk_name(a_dev), "fake") == 0, "the first is named");
    CHECK(strcmp(vibeos_blk_name(b_dev), "second") == 0, "and so is the second");
}

/* --- every path sets a reason --------------------------------------------- */

/* A short transfer is not a success. This is the check that does not exist
 * today, and the defect it answers is one this project has already had. */
static void test_short_transfer(void) {
    uint32_t dev = bd_reset();
    unsigned char buf[BD_SECBYTES * 8u];
    vibeos_blk_request_t req;

    g_fake_short_by = 4;          /* move four of eight, and say it went fine */

    req.device = dev; req.lba = 0; req.sectors = 8;
    req.buf = buf; req.write = 0;
    CHECK(vibeos_blk_submit(&req) != 0, "refused");
    CHECK(req.result == VIBEOS_BLK_SHORT, "and named as short");
    CHECK(req.sectors_done == 4u, "with what did move reported");
    CHECK(vibeos_io_stats()->reads == 0u, "not counted as a read");
}

/* A driver that fails without saying why still fails. Silence is not a
 * success, and it is not the caller's job to guess. */
static void test_silent_failure_becomes_medium(void) {
    uint32_t dev = bd_reset();
    unsigned char buf[BD_SECBYTES];
    vibeos_blk_request_t req;

    g_fake_rc = -1;               /* non-zero, and no reason set */

    req.device = dev; req.lba = 0; req.sectors = 1;
    req.buf = buf; req.write = 0;
    CHECK(vibeos_blk_submit(&req) != 0, "refused");
    CHECK(req.result == VIBEOS_BLK_MEDIUM, "reported as a medium error");
}

/* A driver that does say why is believed - it knows more than this layer. */
static void test_driver_reason_is_kept(void) {
    uint32_t dev = bd_reset();
    unsigned char buf[BD_SECBYTES];
    vibeos_blk_request_t req;

    g_fake_rc = -1;
    g_fake_reason = VIBEOS_BLK_TIMEOUT;

    req.device = dev; req.lba = 0; req.sectors = 1;
    req.buf = buf; req.write = 0;
    CHECK(vibeos_blk_submit(&req) != 0, "refused");
    CHECK(req.result == VIBEOS_BLK_TIMEOUT, "with the driver's own reason");
    CHECK(vibeos_io_stats()->results[VIBEOS_BLK_TIMEOUT] == 1u, "counted as one");
}

/* --- the refusals --------------------------------------------------------- */

/* Past the end of the device, checked here rather than in the driver.
 *
 * A driver that checks its own bounds protects the device; it cannot protect a
 * neighbouring partition, whose sectors are perfectly valid addresses. And the
 * driver must never be reached, because on a real device the transfer would
 * have started before anything noticed. */
static void test_out_of_range(void) {
    uint32_t dev = bd_reset();
    unsigned char buf[BD_SECBYTES * 4u];
    vibeos_blk_request_t req;

    req.device = dev; req.lba = BD_SECTORS - 2u; req.sectors = 4;
    req.buf = buf; req.write = 0;
    CHECK(vibeos_blk_submit(&req) != 0, "refused");
    CHECK(req.result == VIBEOS_BLK_OUT_OF_RANGE, "named");
    CHECK(g_fake_calls == 0u, "and the driver was never asked");

    /* Exactly to the end is allowed - an off-by-one here loses the last
     * sector of every device. */
    req.device = dev; req.lba = BD_SECTORS - 4u; req.sectors = 4;
    req.buf = buf; req.write = 0;
    CHECK(vibeos_blk_submit(&req) == 0, "the last sectors are reachable");
}

static void test_bad_requests(void) {
    uint32_t dev = bd_reset();
    unsigned char buf[BD_SECBYTES];
    vibeos_blk_request_t req;

    req.device = dev; req.lba = 0; req.sectors = 1;
    req.buf = 0; req.write = 0;
    CHECK(vibeos_blk_submit(&req) != 0, "no buffer refused");
    CHECK(req.result == VIBEOS_BLK_BAD_REQUEST, "named");

    req.device = dev; req.lba = 0; req.sectors = 0;
    req.buf = buf; req.write = 0;
    CHECK(vibeos_blk_submit(&req) != 0, "zero sectors refused");
    CHECK(req.result == VIBEOS_BLK_BAD_REQUEST, "named");

    req.device = 99; req.lba = 0; req.sectors = 1;
    req.buf = buf; req.write = 0;
    CHECK(vibeos_blk_submit(&req) != 0, "unknown device refused");
    CHECK(req.result == VIBEOS_BLK_NO_DEVICE, "named");

    CHECK(g_fake_calls == 0u, "and none of them reached the driver");
}

/* A device that cannot describe itself is refused. Guessing a size would make
 * every bounds check afterwards a guess. */
static void test_register_refuses_incomplete(void) {
    vibeos_blk_driver_t d;

    vibeos_blk_reset();
    vibeos_io_stats_reset();

    d = fake_driver("no submit"); d.submit = 0;
    CHECK(vibeos_blk_register(&d, 0) != 0, "no submit refused");

    d = fake_driver("no size");   d.sectors = 0;
    CHECK(vibeos_blk_register(&d, 0) != 0, "no length refused");

    d = fake_driver("no sector"); d.sector_bytes = 0;
    CHECK(vibeos_blk_register(&d, 0) != 0, "no sector size refused");

    CHECK(vibeos_blk_count() == 0u, "nothing was registered");
    CHECK(vibeos_io_stats()->register_refused == 3u, "all three counted");
}

/* A stale reason in the caller's struct is not believed.
 *
 * This is what the reset at the top of submit() is for, and the first version
 * of these tests did not reach it: every refusal path sets a reason explicitly,
 * so removing the reset changed nothing they looked at and the sabotage case
 * walked through a green suite.
 *
 * The path that needs it is the one where a driver fails *silently*. The layer
 * decides "did the driver say why?" by asking whether the result is still
 * NOT_ISSUED - so without the reset, a caller who left TIMEOUT in the struct
 * from a previous request gets that reason attributed to this one, and a
 * medium error is reported as a timeout for the rest of its life. */
static void test_stale_reason_is_not_believed(void) {
    uint32_t dev = bd_reset();
    unsigned char buf[BD_SECBYTES];
    vibeos_blk_request_t req;

    g_fake_rc = -1;                            /* fails, and says nothing */
    g_fake_reason = VIBEOS_BLK_NOT_ISSUED;

    req.device = dev; req.lba = 0; req.sectors = 1;
    req.buf = buf; req.write = 0;
    req.result = VIBEOS_BLK_TIMEOUT;           /* left over from last time */
    req.sectors_done = 7;                      /* and so is this */

    CHECK(vibeos_blk_submit(&req) != 0, "refused");
    CHECK(req.result == VIBEOS_BLK_MEDIUM,
          "the silent failure is a medium error, not the caller's old timeout");
    CHECK(vibeos_io_stats()->results[VIBEOS_BLK_TIMEOUT] == 0u,
          "and nothing was counted as a timeout");
}

/* An untouched request says so rather than looking like a success. The value
 * this replaces was a bare int, where "never sent" and "fine" were both 0. */
static void test_untouched_request(void) {
    vibeos_blk_request_t req;

    vibeos_blk_reset();
    memset(&req, 0, sizeof(req));
    req.result = VIBEOS_BLK_OK;    /* a caller leaving optimism behind */
    req.device = 0; req.lba = 0; req.sectors = 1;
    req.buf = (void *)&req; req.write = 0;

    CHECK(vibeos_blk_submit(&req) != 0, "refused with no devices");
    CHECK(req.result == VIBEOS_BLK_NO_DEVICE,
          "and the caller's stale OK was overwritten");
}

int test_blkdev(void) {
    g_fail = 0;

    test_round_trip();
    test_two_devices();
    test_short_transfer();
    test_silent_failure_becomes_medium();
    test_driver_reason_is_kept();
    test_out_of_range();
    test_bad_requests();
    test_register_refuses_incomplete();
    test_stale_reason_is_not_believed();
    test_untouched_request();

    if (g_fail == 0) {
        printf("  blkdev: 10 groups ok\n");
    }
    return g_fail == 0 ? 0 : 1;
}
