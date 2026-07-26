# Cornix Choc v1 

- Module: ebyte E73-2G4M08S1C (link)[https://www.cdebyte.com/products/E73-2G4M08S1C]
- Soc: nrf52840
- Project name: SP46

version: stock v1 choc

## Clock source

The module vendor has confirmed that this E73-2G4M08S1C board has no
32.768 kHz crystal fitted. Keep the nRF52840 LFCLK on the calibrated RC
source (`CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC` and
`CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC_CALIBRATION`); do not enable the XTAL
source unless a future hardware revision adds that crystal.

## PCB layout

- physical-layout:  https://zmk-physical-layout-converter.streamlit.app/?layout=H4sIAAAAAAACA-2VTUvEMBCG7_srQs9rSWaatvHmXfCiBxER74KwrmAp-99NP7aZlmnGHIUUStOZt28neUKmPyhVfLx3n9_nr-JW9f7VB-7vnh-eHt-sXkKLykde5ohSvSp-fECX-qiKbhxZvbqMuhy3cpMmByIHywiQCDSTryQDm1ZQW9oUuSNyXMt1xS2PJh9w8zGGOm4sG-4DSKvYYJo-8PeVRYHzeSB5gbARCBuJMF9BQMrnHcljnJmRmHkHcPRq48h21hQFQYDireJUdgRABAIWELCAhGWnhFr4Q7MsAp9vSd7GwYIEFiSw3gHjKPdIoKQIMFGCiRJMZCcaSsCyRhZmEDR-dJom73AYB5inbtQ2PG7yj_W5Us2OMO2pPxo2ZV2tDqfZ3pbYbuOD-02avSNr1tirx2rSzm9swcVQeOzSkj3k4QjHAYp7aFLMgtfxOeuvjb2C3NhzY8-NPTf23NhzY8-N_R839sNwX34BxrzaZccPAAA%3D
