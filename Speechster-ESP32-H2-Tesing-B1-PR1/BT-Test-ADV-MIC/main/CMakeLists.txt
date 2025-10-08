idf_component_register(SRCS "BT-Test.c"
	            PRIV_REQUIRES nvs_flash
                    INCLUDE_DIRS "."
	    	    REQUIRES bt
)
