CREATE TABLE public.accounts
(
    id bigserial NOT NULL,
    flags bigint NOT NULL DEFAULT 0,
    name text NOT NULL,
    email text NOT NULL DEFAULT '',
    registration_timestamp bigint NOT NULL,
    last_login_timestamp bigint NOT NULL DEFAULT 0,
    blob bytea,
    PRIMARY KEY (id)
)

TABLESPACE pg_default;

ALTER TABLE IF EXISTS public.accounts
    OWNER to postgres;

ALTER TABLE IF EXISTS public.accounts
    ADD CONSTRAINT unique_name UNIQUE (name);

ALTER TABLE public.characters
    ALTER COLUMN account_id TYPE bigint;