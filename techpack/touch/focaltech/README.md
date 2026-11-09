# Spring FocalTech source port

Base source is Xiaomi SM4450 FT8725 at commit 188826d57006aadbeca2ae35bb2a5baf9ae8b306. The Spring port uses the Tianma panel boot argument, the exact ODM firmware filename, the Spring panel notifier and power-supply charger detection. The Longcheer integration covers firmware info, self-test, work status, proximity, gesture, grip angle and lockdown. The temporary V6 build reproduced the three original export CRCs.

Xiaomi touch registration, the Spring earphone event and MiSight reporting remain open. Hardware operation and the self-test path have not been verified on a phone.
