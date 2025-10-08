idf_component_register(SRCS "BT-Test.c"
	            PRIV_REQUIRES nvs_flash
                    INCLUDE_DIRS "." "$ENV{IDF_PATH}/components/bt/host/nimble/esp-hci/include"
	    	    REQUIRES bt
)
