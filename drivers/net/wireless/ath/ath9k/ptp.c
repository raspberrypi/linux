#include "ath9k.h"

#include <linux/ktime.h>
#include <linux/clocksource.h>
#include <linux/ptp_clock_kernel.h>

static int ath9k_phc_adjfine(struct ptp_clock_info *ptp, long scaled_ppm) {
#if 0
    struct ath_softc *sc = container_of(ptp, struct ath_softc, ptp_clock_info);
    unsigned long flags;
    int neg_adj = 0;
    u32 mult, diff;
    u64 adj;

    if (scaled_ppm < 0) {
        neg_adj = -1;
        scaled_ppm = -scaled_ppm;
    }
    mult = sc->cc_mult;
    adj = mult;
    adj *= scaled_ppm;
    diff = div_u64(adj, 1000000000ULL);

    spin_lock_irqsave(&sc->systim_lock, flags);
    timecounter_read(&sc->tc);
    sc->cc.mult = neg_adj ? mult - diff : mult + diff;
    spin_unlock_irqrestore(&sc->systim_lock, flags);

    ath_warn(ath9k_hw_common(sc->sc_ah), "phc adjust adj=%llu freq=%u\n", adj, diff);
#endif

    return 0;
}

static int ath9k_phc_adjtime(struct ptp_clock_info *ptp, s64 delta) {
    struct ath_softc *sc = container_of(ptp, struct ath_softc, ptp_clock_info);
    unsigned long flags;

    spin_lock_irqsave(&sc->systim_lock, flags);
    timecounter_adjtime(&sc->tc, delta);
    spin_unlock_irqrestore(&sc->systim_lock, flags);

    ath_warn(ath9k_hw_common(sc->sc_ah), "phc adjust abs: %lld\n", delta);

    return 0;
}

static int ath9k_phc_gettime(struct ptp_clock_info *ptp, struct timespec64 *ts) {
    struct ath_softc *sc = container_of(ptp, struct ath_softc, ptp_clock_info);
    unsigned long flags;
    u64 ns;

    spin_lock_irqsave(&sc->systim_lock, flags);
    ns = timecounter_read(&sc->tc);
    spin_unlock_irqrestore(&sc->systim_lock, flags);

    *ts = ns_to_timespec64(ns);

    return 0;
}

static int ath9k_phc_settime(struct ptp_clock_info *ptp,
                const struct timespec64 *ts) {
    struct ath_softc *sc = container_of(ptp, struct ath_softc, ptp_clock_info);
    unsigned long flags;
    u64 ns;

    ns = timespec64_to_ns(ts);
    spin_lock_irqsave(&sc->systim_lock, flags);
    timecounter_init(&sc->tc, &sc->cc, ns);
    spin_unlock_irqrestore(&sc->systim_lock, flags);

    return 0;
}

static int ath9k_phc_enable(struct ptp_clock_info __always_unused *ptp,
                struct ptp_clock_request __always_unused *request,
                int __always_unused on) {
    return -EOPNOTSUPP;
}

static const struct ptp_clock_info ath9k_ptp_clock_info = {
    .owner      = THIS_MODULE,
    .n_alarm    = 0,
    .n_ext_ts   = 0,
    .n_per_out  = 0,
    .n_pins     = 0,
    .pps        = 0,
    .adjfine    = ath9k_phc_adjfine,
    .adjtime    = ath9k_phc_adjtime,
    .gettime64  = ath9k_phc_gettime,
    .settime64  = ath9k_phc_settime,
    .enable     = ath9k_phc_enable,
};

void ath9k_ptp_init(struct ath_softc *sc) {
    sc->ptp_clock = NULL;

    sc->ptp_clock_info = ath9k_ptp_clock_info;

    snprintf(sc->ptp_clock_info.name,
        sizeof(sc->ptp_clock_info.name), "%pm",
        sc->hw->wiphy->perm_addr);

    sc->ptp_clock_info.max_adj = 1e6;

    sc->ptp_clock = ptp_clock_register(&sc->ptp_clock_info, sc->dev);

    if (IS_ERR(sc->ptp_clock)) {
        sc->ptp_clock = NULL;
        ath_err(ath9k_hw_common(sc->sc_ah), "ptp_clock_register failed\n");
    } else if (sc->ptp_clock) {
        ath_info(ath9k_hw_common(sc->sc_ah), "registered PHC clock\n");
    }
}

void ath9k_ptp_remove(struct ath_softc *sc) {
    if (sc->ptp_clock) {
        ptp_clock_unregister(sc->ptp_clock);
        sc->ptp_clock = NULL;
        ath_info(ath9k_hw_common(sc->sc_ah), "removed PHC clock\n");
    }
}

